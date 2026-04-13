#ifndef GALAY_HTTP_SESSION_H
#define GALAY_HTTP_SESSION_H

#include "HttpLog.h"
#include "HttpReader.h"
#include "HttpWriter.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/common/Buffer.h"
#include "galay-kernel/kernel/Awaitable.h"
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/async/SslAwaitableCore.h"
#include "galay-ssl/async/SslSocket.h"
#endif

namespace galay::http {

using namespace galay::async;
using namespace galay::kernel;

template<typename SocketType>
class HttpSessionImpl;

namespace detail {

template<typename SocketType>
struct HttpSessionState {
    using ResultType = std::expected<std::optional<HttpResponse>, HttpError>;

    HttpSessionState(HttpSessionImpl<SocketType>& session, HttpRequest&& request)
        : m_session(&session)
        , m_request(std::move(request))
        , m_send_buffer(m_request.toString()) {}

    HttpSessionState(HttpSessionImpl<SocketType>& session, std::string&& serialized_request)
        : m_session(&session)
        , m_send_buffer(std::move(serialized_request)) {}

    bool sendCompleted() const {
        return m_send_offset >= m_send_buffer.size();
    }

    const char* sendBuffer() const {
        return m_send_buffer.data() + m_send_offset;
    }

    size_t sendRemaining() const {
        return m_send_buffer.size() - m_send_offset;
    }

    void onBytesSent(size_t sent) {
        m_send_offset += sent;
    }

    bool parseFromRingBuffer() {
        auto read_iovecs = borrowReadIovecs(m_session->getRingBuffer());
        if (read_iovecs.empty()) {
            return false;
        }

        if (IoVecWindow::buildWindow(read_iovecs, m_parse_iovecs) == 0) {
            return false;
        }

        auto [error_code, consumed] = m_response.fromIOVec(m_parse_iovecs);
        if (consumed > 0) {
            m_session->getRingBuffer().consume(static_cast<size_t>(consumed));
        }

        if (error_code == kHeaderInComplete || error_code == kIncomplete) {
            if (m_total_received >= m_session->getReaderSetting().getMaxHeaderSize() &&
                !m_response.isComplete()) {
                setParseError(HttpError(kHeaderTooLarge));
                return true;
            }
            return false;
        }

        if (error_code != kNoError) {
            setParseError(HttpError(error_code));
            return true;
        }

        if (!m_response.isComplete()) {
            return false;
        }

        m_response_value = std::optional<HttpResponse>(std::move(m_response));
        return true;
    }

    bool prepareRecvWindow() {
        m_write_iovecs = borrowWriteIovecs(m_session->getRingBuffer());
        if (m_write_iovecs.empty()) {
            setParseError(HttpError(kHeaderTooLarge));
            return false;
        }
        return true;
    }

    bool prepareRecvWindow(char*& buffer, size_t& length) {
        if (!prepareRecvWindow()) {
            buffer = nullptr;
            length = 0;
            return false;
        }
        if (!IoVecWindow::bindFirstNonEmpty(m_write_iovecs, buffer, length)) {
            setParseError(HttpError(kHeaderTooLarge));
            return false;
        }
        return true;
    }

    const struct iovec* recvIovecsData() const { return m_write_iovecs.data(); }
    size_t recvIovecsCount() const { return m_write_iovecs.size(); }

    void setSendError(const IOError& io_error) {
        if (IOError::contains(io_error.code(), kTimeout)) {
            m_error = HttpError(kRequestTimeOut, io_error.message());
            return;
        }
        m_error = HttpError(kSendError, io_error.message());
    }

    void setRecvError(const IOError& io_error) {
        if (IOError::contains(io_error.code(), kTimeout)) {
            m_error = HttpError(kRequestTimeOut, io_error.message());
            return;
        }
        if (IOError::contains(io_error.code(), kDisconnectError)) {
            m_error = HttpError(kConnectionClose);
            return;
        }
        m_error = HttpError(kTcpRecvError, io_error.message());
    }

#ifdef GALAY_HTTP_SSL_ENABLED
    void setSslSendError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            m_error = HttpError(kConnectionClose, error.message());
            return;
        }
        m_error = HttpError(kSendError, error.message());
    }

    void setSslRecvError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            m_error = HttpError(kConnectionClose, error.message());
            return;
        }
        m_error = HttpError(kRecvError, error.message());
    }
#endif

    void onPeerClosed() {
        m_error = HttpError(kConnectionClose);
    }

    void onBytesReceived(size_t recv_bytes) {
        m_session->getRingBuffer().produce(recv_bytes);
        m_total_received += recv_bytes;
    }

    void setParseError(HttpError&& error) {
        m_error = std::move(error);
    }

    ResultType takeResult() {
        if (m_error.has_value()) {
            return std::unexpected(std::move(*m_error));
        }
        if (m_response_value.has_value()) {
            return std::move(*m_response_value);
        }
        return std::optional<HttpResponse>{};
    }

    HttpSessionImpl<SocketType>* m_session;
    HttpRequest m_request;
    HttpResponse m_response;
    std::string m_send_buffer;
    size_t m_send_offset = 0;
    size_t m_total_received = 0;
    std::vector<iovec> m_parse_iovecs;
    BorrowedIovecs<2> m_write_iovecs;
    std::optional<HttpError> m_error;
    std::optional<std::optional<HttpResponse>> m_response_value;
};

template<typename SocketType>
struct HttpSessionTcpMachine {
    using result_type = typename HttpSessionState<SocketType>::ResultType;

    explicit HttpSessionTcpMachine(std::shared_ptr<HttpSessionState<SocketType>> state)
        : m_state(std::move(state)) {}

    MachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        if (!m_state->sendCompleted()) {
            return MachineAction<result_type>::waitWrite(
                m_state->sendBuffer(),
                m_state->sendRemaining());
        }

        if (m_state->parseFromRingBuffer()) {
            m_result = m_state->takeResult();
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        if (!m_state->prepareRecvWindow()) {
            m_result = m_state->takeResult();
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        return MachineAction<result_type>::waitReadv(
            m_state->recvIovecsData(),
            m_state->recvIovecsCount());
    }

    void onRead(std::expected<size_t, IOError> result) {
        if (!result) {
            m_state->setRecvError(result.error());
            m_result = m_state->takeResult();
            return;
        }

        if (result.value() == 0) {
            m_state->onPeerClosed();
            m_result = m_state->takeResult();
            return;
        }

        m_state->onBytesReceived(result.value());
    }

    void onWrite(std::expected<size_t, IOError> result) {
        if (!result) {
            m_state->setSendError(result.error());
            m_result = m_state->takeResult();
            return;
        }

        m_state->onBytesSent(result.value());
    }

    std::shared_ptr<HttpSessionState<SocketType>> m_state;
    std::optional<result_type> m_result;
};

#ifdef GALAY_HTTP_SSL_ENABLED
template<typename SocketType>
struct HttpSessionSslMachine {
    using result_type = typename HttpSessionState<SocketType>::ResultType;

    explicit HttpSessionSslMachine(std::shared_ptr<HttpSessionState<SocketType>> state)
        : m_state(std::move(state)) {}

    galay::ssl::SslMachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        if (!m_state->sendCompleted()) {
            return galay::ssl::SslMachineAction<result_type>::send(
                m_state->sendBuffer(),
                m_state->sendRemaining());
        }

        if (m_state->parseFromRingBuffer()) {
            m_result = m_state->takeResult();
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        char* recv_buffer = nullptr;
        size_t recv_length = 0;
        if (!m_state->prepareRecvWindow(recv_buffer, recv_length)) {
            m_result = m_state->takeResult();
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        return galay::ssl::SslMachineAction<result_type>::recv(recv_buffer, recv_length);
    }

    void onHandshake(std::expected<void, galay::ssl::SslError>) {}

    void onRecv(std::expected<Bytes, galay::ssl::SslError> result) {
        if (!result) {
            m_state->setSslRecvError(result.error());
            m_result = m_state->takeResult();
            return;
        }

        const size_t recv_bytes = result.value().size();
        if (recv_bytes == 0) {
            m_state->onPeerClosed();
            m_result = m_state->takeResult();
            return;
        }

        m_state->onBytesReceived(recv_bytes);
    }

    void onSend(std::expected<size_t, galay::ssl::SslError> result) {
        if (!result) {
            m_state->setSslSendError(result.error());
            m_result = m_state->takeResult();
            return;
        }

        m_state->onBytesSent(result.value());
    }

    void onShutdown(std::expected<void, galay::ssl::SslError>) {}

    std::shared_ptr<HttpSessionState<SocketType>> m_state;
    std::optional<result_type> m_result;
};
#endif

template<typename SocketType>
auto buildSessionOperation(HttpSessionImpl<SocketType>& session, HttpRequest&& request) {
    using State = HttpSessionState<SocketType>;
    using ResultType = typename State::ResultType;
    auto state = std::make_shared<State>(session, std::move(request));

    if constexpr (std::is_same_v<SocketType, TcpSocket>) {
        return AwaitableBuilder<ResultType>::fromStateMachine(
                   session.getSocket().controller(),
                   HttpSessionTcpMachine<SocketType>(std::move(state)))
            .build();
    } else {
#ifdef GALAY_HTTP_SSL_ENABLED
        return galay::ssl::SslAwaitableBuilder<ResultType>::fromStateMachine(
                   session.getSocket().controller(),
                   &session.getSocket(),
                   HttpSessionSslMachine<SocketType>(std::move(state)))
            .build();
#else
        static_assert(!sizeof(SocketType), "SSL support is disabled");
#endif
    }
}

template<typename SocketType>
auto buildSessionOperation(HttpSessionImpl<SocketType>& session, std::string&& serialized_request) {
    using State = HttpSessionState<SocketType>;
    using ResultType = typename State::ResultType;
    auto state = std::make_shared<State>(session, std::move(serialized_request));

    if constexpr (std::is_same_v<SocketType, TcpSocket>) {
        return AwaitableBuilder<ResultType>::fromStateMachine(
                   session.getSocket().controller(),
                   HttpSessionTcpMachine<SocketType>(std::move(state)))
            .build();
    } else {
#ifdef GALAY_HTTP_SSL_ENABLED
        return galay::ssl::SslAwaitableBuilder<ResultType>::fromStateMachine(
                   session.getSocket().controller(),
                   &session.getSocket(),
                   HttpSessionSslMachine<SocketType>(std::move(state)))
            .build();
#else
        static_assert(!sizeof(SocketType), "SSL support is disabled");
#endif
    }
}

} // namespace detail

template<typename SocketType>
class HttpSessionImpl {
public:
    HttpSessionImpl(SocketType& socket,
                    size_t ring_buffer_size = 8192,
                    const HttpReaderSetting& reader_setting = HttpReaderSetting(),
                    const HttpWriterSetting& writer_setting = HttpWriterSetting())
        : m_socket(socket)
        , m_ring_buffer(ring_buffer_size)
        , m_reader_setting(reader_setting)
        , m_writer_setting(writer_setting)
        , m_reader(m_ring_buffer, m_reader_setting, socket)
        , m_writer(m_writer_setting, socket) {}

    HttpReaderImpl<SocketType>& getReader() { return m_reader; }

    HttpWriterImpl<SocketType>& getWriter() { return m_writer; }

    SocketType& getSocket() { return m_socket; }

    RingBuffer& getRingBuffer() { return m_ring_buffer; }

    const HttpReaderSetting& getReaderSetting() const { return m_reader_setting; }

    auto get(const std::string& uri,
             const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::GET, uri, "", "", headers);
    }

    auto post(const std::string& uri,
              const std::string& body,
              const std::string& content_type = "application/x-www-form-urlencoded",
              const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::POST, uri, body, content_type, headers);
    }

    /**
     * @brief 发送带右值请求体的 POST 请求
     * @param uri 请求 URI
     * @param body 调用方可转移所有权的请求体
     * @param content_type 请求体 Content-Type
     * @param headers 额外请求头
     * @return 请求-响应一体化 awaitable
     * @note 该重载会把请求体直接移动进内部 HttpRequest，适合热点路径避免额外 body 拷贝
     */
    auto post(const std::string& uri,
              std::string&& body,
              const std::string& content_type = "application/x-www-form-urlencoded",
              const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::POST, uri, std::move(body), content_type, headers);
    }

    auto put(const std::string& uri,
             const std::string& body,
             const std::string& content_type = "application/json",
             const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::PUT, uri, body, content_type, headers);
    }

    auto del(const std::string& uri,
             const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::DELETE, uri, "", "", headers);
    }

    auto head(const std::string& uri,
              const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::HEAD, uri, "", "", headers);
    }

    auto options(const std::string& uri,
                 const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::OPTIONS, uri, "", "", headers);
    }

    auto patch(const std::string& uri,
               const std::string& body,
               const std::string& content_type = "application/json",
               const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::PATCH, uri, body, content_type, headers);
    }

    auto trace(const std::string& uri,
               const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::TRACE, uri, "", "", headers);
    }

    auto tunnel(const std::string& target_host,
                const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::CONNECT, target_host, "", "", headers);
    }

    auto sendRequest(HttpRequest& request) {
        return m_writer.sendRequest(request);
    }

    /**
     * @brief 发送调用方已预先序列化好的完整 HTTP/1.x 请求字节
     * @param request 包含 start-line、headers、空行与 body 的完整请求报文
     * @return 请求-响应一体化 awaitable
     * @note 该接口复用 HttpSession 的超时、收包与响应解析状态机，但跳过 HttpRequest/Header 序列化
     * @note request 的所有权会转移到 awaitable 内部；await 完成前无需额外保持外部缓冲存活
     * @note 调用方必须自行保证报文格式合法，尤其是 Content-Length、Connection 与请求行
     */
    auto sendSerializedRequest(std::string request) {
        return detail::buildSessionOperation(*this, std::move(request));
    }

    auto getResponse(HttpResponse& response) {
        return m_reader.getResponse(response);
    }

    auto sendChunk(const std::string& data, bool is_last = false) {
        return m_writer.sendChunk(data, is_last);
    }

private:
    auto createRequest(HttpMethod method,
                       const std::string& uri,
                       std::string body,
                       const std::string& content_type,
                       const std::map<std::string, std::string>& headers) {
        HttpRequest request;
        HttpRequestHeader header;

        header.method() = method;
        header.uri() = uri;
        header.version() = HttpVersion::HttpVersion_1_1;

        if (!body.empty() && !content_type.empty()) {
            header.headerPairs().addHeaderPair("Content-Type", content_type);
            header.headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));
        }

        for (const auto& [key, value] : headers) {
            header.headerPairs().addHeaderPair(key, value);
        }

        request.setHeader(std::move(header));
        if (!body.empty()) {
            request.setBodyStr(std::move(body));
        }

        return detail::buildSessionOperation(*this, std::move(request));
    }

    SocketType& m_socket;
    RingBuffer m_ring_buffer;
    HttpReaderSetting m_reader_setting;
    HttpWriterSetting m_writer_setting;
    HttpReaderImpl<SocketType> m_reader;
    HttpWriterImpl<SocketType> m_writer;
};

using HttpSession = HttpSessionImpl<TcpSocket>;

} // namespace galay::http

#ifdef GALAY_HTTP_SSL_ENABLED
namespace galay::http {
using HttpsSession = HttpSessionImpl<galay::ssl::SslSocket>;
} // namespace galay::http
#endif

#endif // GALAY_HTTP_SESSION_H
