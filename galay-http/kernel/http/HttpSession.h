#ifndef GALAY_HTTP_SESSION_H
#define GALAY_HTTP_SESSION_H

#include "HttpWriter.h"
#include "HttpReader.h"
#include "HttpLog.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/common/Buffer.h"
#include "galay-kernel/kernel/IOHandlers.hpp"
#include "galay-kernel/kernel/Awaitable.h"
#include "galay-kernel/kernel/Timeout.hpp"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include <string>
#include <optional>
#include <coroutine>
#include <map>
#include <type_traits>
#include <vector>

namespace galay::http
{

using namespace galay::async;
using namespace galay::kernel;

// 前向声明
template<typename SocketType>
class HttpSessionImpl;

/**
 * @brief HTTP会话等待体模板类
 */
template<typename SocketType, bool IsTcp = std::is_same_v<SocketType, TcpSocket>>
class HttpSessionAwaitableImpl;

template<typename T>
struct dependent_false : std::false_type {};

// 非 TcpSocket 的兜底版本：要求提供专用特化（例如 SslSocket）
template<typename SocketType>
class HttpSessionAwaitableImpl<SocketType, false> : public galay::kernel::TimeoutSupport<HttpSessionAwaitableImpl<SocketType, false>>
{
public:
    HttpSessionAwaitableImpl(HttpSessionImpl<SocketType>&, HttpRequest&&) {
        static_assert(dependent_false<SocketType>::value,
                      "HttpSessionAwaitableImpl<SocketType,false> requires a socket-specific specialization");
    }

    bool await_ready() const noexcept { return true; }
    void await_suspend(std::coroutine_handle<>) const noexcept {}

    std::expected<std::optional<HttpResponse>, HttpError> await_resume() {
        return std::unexpected(HttpError(kInternalError, "Unsupported socket type for HttpSession"));
    }

    bool isInvalid() const { return true; }

public:
    std::expected<std::optional<HttpResponse>, galay::kernel::IOError> m_result;
};

// TcpSocket 版本：使用 CustomAwaitable 实现 send + recv 链式等待
template<typename SocketType>
class HttpSessionAwaitableImpl<SocketType, true>
    : public galay::kernel::CustomAwaitable
    , public galay::kernel::TimeoutSupport<HttpSessionAwaitableImpl<SocketType, true>>
{
public:
    class ProtocolSendAwaitable : public galay::kernel::SendAwaitable
    {
    public:
        explicit ProtocolSendAwaitable(HttpSessionAwaitableImpl* owner)
            : galay::kernel::SendAwaitable(owner->m_session->getSocket().controller(),
                                          owner->m_send_buffer.data(),
                                          owner->m_send_buffer.size())
            , m_owner(owner)
        {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle) override {
            if (m_length == 0) {
                return true;
            }
            if (cqe == nullptr) {
                return false;
            }

            auto result = galay::kernel::io::handleSend(cqe);
            if (!result && galay::kernel::IOError::contains(result.error().code(), galay::kernel::kNotReady)) {
                return false;
            }
            if (!result) {
                m_owner->setSendError(result.error());
                return true;
            }

            size_t sent = result.value();
            if (sent == 0) {
                return false;
            }

            m_buffer += sent;
            m_length -= sent;
            return m_length == 0;
        }
#else
        bool handleComplete(GHandle handle) override {
            while (m_length > 0) {
                auto result = galay::kernel::io::handleSend(handle, m_buffer, m_length);
                if (!result && galay::kernel::IOError::contains(result.error().code(), galay::kernel::kNotReady)) {
                    return false;
                }
                if (!result) {
                    m_owner->setSendError(result.error());
                    return true;
                }

                size_t sent = result.value();
                if (sent == 0) {
                    return false;
                }

                m_buffer += sent;
                m_length -= sent;
            }
            return true;
        }
#endif

    private:
        HttpSessionAwaitableImpl* m_owner;
    };

    class ProtocolRecvAwaitable : public galay::kernel::RecvAwaitable
    {
    public:
        explicit ProtocolRecvAwaitable(HttpSessionAwaitableImpl* owner)
            : galay::kernel::RecvAwaitable(owner->m_session->getSocket().controller(), nullptr, 0)
            , m_owner(owner)
        {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle) override {
            if (m_owner->parseResponseFromRingBuffer()) {
                return true;
            }

            if (cqe == nullptr) {
                if (!prepareRecvWindow()) {
                    m_owner->setRecvError(HttpError(kHeaderTooLarge));
                    return true;
                }
                return false;
            }

            auto result = galay::kernel::io::handleRecv(cqe, m_buffer);
            if (!result && galay::kernel::IOError::contains(result.error().code(), galay::kernel::kNotReady)) {
                return false;
            }
            if (!result) {
                m_owner->setRecvError(result.error());
                return true;
            }

            size_t recv_bytes = result.value();
            if (recv_bytes == 0) {
                m_owner->setRecvError(HttpError(kConnectionClose));
                return true;
            }

            m_owner->m_session->getRingBuffer().produce(recv_bytes);
            m_owner->m_total_received += recv_bytes;

            if (m_owner->parseResponseFromRingBuffer()) {
                return true;
            }

            if (!prepareRecvWindow()) {
                m_owner->setRecvError(HttpError(kHeaderTooLarge));
                return true;
            }
            return false;
        }
#else
        bool handleComplete(GHandle handle) override {
            if (m_owner->parseResponseFromRingBuffer()) {
                return true;
            }

            while (true) {
                if (!prepareRecvWindow()) {
                    m_owner->setRecvError(HttpError(kHeaderTooLarge));
                    return true;
                }

                auto result = galay::kernel::io::handleRecv(handle, m_buffer, m_length);
                if (!result && galay::kernel::IOError::contains(result.error().code(), galay::kernel::kNotReady)) {
                    return false;
                }
                if (!result) {
                    m_owner->setRecvError(result.error());
                    return true;
                }

                size_t recv_bytes = result.value();
                if (recv_bytes == 0) {
                    m_owner->setRecvError(HttpError(kConnectionClose));
                    return true;
                }

                m_owner->m_session->getRingBuffer().produce(recv_bytes);
                m_owner->m_total_received += recv_bytes;

                if (m_owner->parseResponseFromRingBuffer()) {
                    return true;
                }
            }
        }
#endif

    private:
        bool prepareRecvWindow() {
            auto write_iovecs = borrowWriteIovecs(m_owner->m_session->getRingBuffer());
            if (write_iovecs.empty()) {
                return false;
            }

            m_buffer = static_cast<char*>(write_iovecs[0].iov_base);
            m_length = write_iovecs[0].iov_len;
            return m_length > 0;
        }

        HttpSessionAwaitableImpl* m_owner;
    };

    HttpSessionAwaitableImpl(HttpSessionImpl<SocketType>& session, HttpRequest&& request)
        : galay::kernel::CustomAwaitable(session.getSocket().controller())
        , m_session(&session)
        , m_request(std::move(request))
        , m_send_buffer(m_request.toString())
        , m_total_received(0)
        , m_state(State::Running)
        , m_send_awaitable(this)
        , m_recv_awaitable(this)
        , m_result(std::nullopt)
    {
        addTask(IOEventType::SEND, &m_send_awaitable);
        addTask(IOEventType::RECV, &m_recv_awaitable);
    }

    bool await_ready() const noexcept {
        return false;
    }

    using galay::kernel::CustomAwaitable::await_suspend;

    std::expected<std::optional<HttpResponse>, HttpError> await_resume() {
        onCompleted();

        if (m_state == State::Invalid) {
            return std::unexpected(HttpError(kInternalError, "HttpSessionAwaitable in Invalid state"));
        }

        if (!m_result.has_value()) {
            auto& io_error = m_result.error();
            HttpErrorCode http_error_code = kTcpRecvError;
            if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kTimeout)) {
                http_error_code = kRequestTimeOut;
            } else if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
                http_error_code = kConnectionClose;
            }

            reset();
            return std::unexpected(HttpError(http_error_code, io_error.message()));
        }

        if (m_http_error.has_value()) {
            auto error = std::move(m_http_error.value());
            reset();
            return std::unexpected(std::move(error));
        }

        auto response = std::move(m_response);
        reset();
        return std::optional<HttpResponse>(std::move(response));
    }

    bool isInvalid() const {
        return m_state == State::Invalid;
    }

private:
    enum class State {
        Running,
        Invalid
    };

    bool parseResponseFromRingBuffer() {
        auto read_iovecs = borrowReadIovecs(m_session->getRingBuffer());
        if (read_iovecs.empty()) {
            return false;
        }

        std::vector<iovec> parse_iovecs;
        if (IoVecWindow::buildWindow(read_iovecs, parse_iovecs) == 0) {
            return false;
        }

        auto [error_code, consumed] = m_response.fromIOVec(parse_iovecs);
        if (consumed > 0) {
            m_session->getRingBuffer().consume(static_cast<size_t>(consumed));
        }

        if (error_code == kHeaderInComplete || error_code == kIncomplete) {
            if (m_total_received >= m_session->getReaderSetting().getMaxHeaderSize() && !m_response.isComplete()) {
                setRecvError(HttpError(kHeaderTooLarge));
                return true;
            }
            return false;
        }

        if (error_code != kNoError) {
            setRecvError(HttpError(error_code));
            return true;
        }

        return m_response.isComplete();
    }

    void setSendError(const galay::kernel::IOError& io_error) {
        m_http_error = HttpError(kSendError, io_error.message());
    }

    void setRecvError(const galay::kernel::IOError& io_error) {
        if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
            m_http_error = HttpError(kConnectionClose);
            return;
        }
        m_http_error = HttpError(kRecvError, io_error.message());
    }

    void setRecvError(HttpError&& error) {
        m_http_error = std::move(error);
    }

    void reset() {
        m_state = State::Invalid;
        m_http_error.reset();
        m_send_buffer.clear();
        m_response.reset();
        m_result = std::nullopt;
    }

    HttpSessionImpl<SocketType>* m_session;
    HttpRequest m_request;
    HttpResponse m_response;
    std::string m_send_buffer;
    size_t m_total_received;
    State m_state;
    ProtocolSendAwaitable m_send_awaitable;
    ProtocolRecvAwaitable m_recv_awaitable;
    std::optional<HttpError> m_http_error;

public:
    std::expected<std::optional<HttpResponse>, galay::kernel::IOError> m_result;
};

/**
 * @brief HTTP会话模板类
 * @details 持有 socket、ring_buffer、reader 和 writer，负责实际的 HTTP 通信
 */
template<typename SocketType>
class HttpSessionImpl
{
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
        , m_writer(m_writer_setting, socket)
    {
    }

    HttpReaderImpl<SocketType>& getReader() {
        return m_reader;
    }

    HttpWriterImpl<SocketType>& getWriter() {
        return m_writer;
    }

    SocketType& getSocket() {
        return m_socket;
    }

    RingBuffer& getRingBuffer() {
        return m_ring_buffer;
    }

    const HttpReaderSetting& getReaderSetting() const {
        return m_reader_setting;
    }

    // 便捷方法：GET 请求
    HttpSessionAwaitableImpl<SocketType> get(const std::string& uri,
                                             const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::GET, uri, "", "", headers);
    }

    // 便捷方法：POST 请求
    HttpSessionAwaitableImpl<SocketType> post(const std::string& uri,
                                              const std::string& body,
                                              const std::string& content_type = "application/x-www-form-urlencoded",
                                              const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::POST, uri, body, content_type, headers);
    }

    // 便捷方法：PUT 请求
    HttpSessionAwaitableImpl<SocketType> put(const std::string& uri,
                                             const std::string& body,
                                             const std::string& content_type = "application/json",
                                             const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::PUT, uri, body, content_type, headers);
    }

    // 便捷方法：DELETE 请求
    HttpSessionAwaitableImpl<SocketType> del(const std::string& uri,
                                             const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::DELETE, uri, "", "", headers);
    }

    // 便捷方法：HEAD 请求
    HttpSessionAwaitableImpl<SocketType> head(const std::string& uri,
                                              const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::HEAD, uri, "", "", headers);
    }

    // 便捷方法：OPTIONS 请求
    HttpSessionAwaitableImpl<SocketType> options(const std::string& uri,
                                                 const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::OPTIONS, uri, "", "", headers);
    }

    // 便捷方法：PATCH 请求
    HttpSessionAwaitableImpl<SocketType> patch(const std::string& uri,
                                               const std::string& body,
                                               const std::string& content_type = "application/json",
                                               const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::PATCH, uri, body, content_type, headers);
    }

    // 便捷方法：TRACE 请求
    HttpSessionAwaitableImpl<SocketType> trace(const std::string& uri,
                                               const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::TRACE, uri, "", "", headers);
    }

    // 便捷方法：CONNECT 请求（用于隧道）
    HttpSessionAwaitableImpl<SocketType> tunnel(const std::string& target_host,
                                                const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::CONNECT, target_host, "", "", headers);
    }

    // 发送请求
    SendResponseAwaitableImpl<SocketType> sendRequest(HttpRequest& request) {
        return m_writer.sendRequest(request);
    }

    // 接收响应
    GetResponseAwaitableImpl<SocketType> getResponse(HttpResponse& response) {
        return m_reader.getResponse(response);
    }

    // 发送分块数据
    SendResponseAwaitableImpl<SocketType> sendChunk(const std::string& data, bool is_last = false) {
        return m_writer.sendChunk(data, is_last);
    }

private:
    HttpSessionAwaitableImpl<SocketType> createRequest(HttpMethod method,
                                                       const std::string& uri,
                                                       const std::string& body,
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
            std::string body_copy = body;
            request.setBodyStr(std::move(body_copy));
        }

        return HttpSessionAwaitableImpl<SocketType>(*this, std::move(request));
    }

    SocketType& m_socket;
    RingBuffer m_ring_buffer;
    HttpReaderSetting m_reader_setting;
    HttpWriterSetting m_writer_setting;
    HttpReaderImpl<SocketType> m_reader;
    HttpWriterImpl<SocketType> m_writer;
};

// 类型别名 - HTTP (TcpSocket)
using HttpSessionAwaitable = HttpSessionAwaitableImpl<TcpSocket>;
using HttpSession = HttpSessionImpl<TcpSocket>;

} // namespace galay::http

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/async/SslSocket.h"
#include "galay-http/kernel/SslRecvCompatAwaitable.h"

namespace galay::http {

template<>
class HttpSessionAwaitableImpl<galay::ssl::SslSocket, false>
    : public galay::kernel::CustomAwaitable
    , public galay::kernel::TimeoutSupport<HttpSessionAwaitableImpl<galay::ssl::SslSocket, false>>
{
public:
    using SocketType = galay::ssl::SslSocket;
    using SendAwaitableType = decltype(std::declval<SocketType>().send(std::declval<const char*>(), std::declval<size_t>()));
    using RecvAwaitableType = galay::http::SslRecvCompatAwaitable;

    class ProtocolSendAwaitable : public SendAwaitableType
    {
    public:
        explicit ProtocolSendAwaitable(HttpSessionAwaitableImpl* owner)
            : SendAwaitableType(owner->m_session->getSocket().send(owner->m_send_buffer.data(),
                                                                   owner->m_send_buffer.size()))
            , m_owner(owner)
        {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override {
            if (m_owner->m_send_completed) {
                return true;
            }
            if (cqe == nullptr) {
                return false;
            }

            this->m_sslResultSet = false;
            const bool done = SendAwaitableType::handleComplete(cqe, handle);
            if (!done) {
                return false;
            }

            auto send_result = std::move(this->m_sslResult);
            this->m_sslResultSet = false;
            if (!send_result) {
                const auto& error = send_result.error();
                if (error.sslError() == SSL_ERROR_WANT_READ || error.sslError() == SSL_ERROR_WANT_WRITE) {
                    return false;
                }
                m_owner->setSslSendError(error);
                return true;
            }

            const size_t sent = send_result.value();
            if (sent == 0) {
                return false;
            }

            m_owner->m_send_offset += sent;
            if (m_owner->m_send_offset >= m_owner->m_send_buffer.size()) {
                m_owner->m_send_completed = true;
                return true;
            }

            this->m_plainBuffer = m_owner->m_send_buffer.data() + m_owner->m_send_offset;
            this->m_plainLength = m_owner->m_send_buffer.size() - m_owner->m_send_offset;
            return false;
        }
#else
        bool handleComplete(GHandle handle) override {
            while (!m_owner->m_send_completed) {
                this->m_sslResultSet = false;
                const bool done = SendAwaitableType::handleComplete(handle);
                if (!done) {
                    return false;
                }

                auto send_result = std::move(this->m_sslResult);
                this->m_sslResultSet = false;
                if (!send_result) {
                    const auto& error = send_result.error();
                    if (error.sslError() == SSL_ERROR_WANT_READ || error.sslError() == SSL_ERROR_WANT_WRITE) {
                        return false;
                    }
                    m_owner->setSslSendError(error);
                    return true;
                }

                const size_t sent = send_result.value();
                if (sent == 0) {
                    return false;
                }

                m_owner->m_send_offset += sent;
                if (m_owner->m_send_offset >= m_owner->m_send_buffer.size()) {
                    m_owner->m_send_completed = true;
                    return true;
                }

                this->m_plainBuffer = m_owner->m_send_buffer.data() + m_owner->m_send_offset;
                this->m_plainLength = m_owner->m_send_buffer.size() - m_owner->m_send_offset;
            }
            return true;
        }
#endif

    private:
        HttpSessionAwaitableImpl* m_owner;
    };

    class ProtocolRecvAwaitable : public RecvAwaitableType
    {
    public:
        explicit ProtocolRecvAwaitable(HttpSessionAwaitableImpl* owner)
            : RecvAwaitableType(owner->m_session->getSocket().recv(owner->m_dummy_recv_buffer,
                                                                   sizeof(owner->m_dummy_recv_buffer)))
            , m_owner(owner)
        {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override {
            if (m_owner->parseResponseFromRingBuffer()) {
                return true;
            }

            if (!prepareRecvWindow()) {
                m_owner->setRecvError(HttpError(kHeaderTooLarge));
                return true;
            }

            if (cqe == nullptr) {
                return false;
            }

            this->m_sslResultSet = false;
            const bool done = RecvAwaitableType::handleComplete(cqe, handle);
            if (!done) {
                return false;
            }

            auto recv_result = std::move(this->m_sslResult);
            this->m_sslResultSet = false;
            if (!recv_result) {
                const auto& error = recv_result.error();
                if (error.sslError() == SSL_ERROR_WANT_READ || error.sslError() == SSL_ERROR_WANT_WRITE) {
                    return false;
                }
                m_owner->setSslRecvError(error);
                return true;
            }

            const size_t recv_bytes = recv_result.value().size();
            if (recv_bytes == 0) {
                m_owner->setRecvError(HttpError(kConnectionClose));
                return true;
            }

            m_owner->m_session->getRingBuffer().produce(recv_bytes);
            m_owner->m_total_received += recv_bytes;
            return m_owner->parseResponseFromRingBuffer();
        }
#else
        bool handleComplete(GHandle handle) override {
            while (true) {
                if (m_owner->parseResponseFromRingBuffer()) {
                    return true;
                }

                if (!prepareRecvWindow()) {
                    m_owner->setRecvError(HttpError(kHeaderTooLarge));
                    return true;
                }

                this->m_sslResultSet = false;
                const bool done = RecvAwaitableType::handleComplete(handle);
                if (!done) {
                    return false;
                }

                auto recv_result = std::move(this->m_sslResult);
                this->m_sslResultSet = false;
                if (!recv_result) {
                    const auto& error = recv_result.error();
                    if (error.sslError() == SSL_ERROR_WANT_READ || error.sslError() == SSL_ERROR_WANT_WRITE) {
                        return false;
                    }
                    m_owner->setSslRecvError(error);
                    return true;
                }

                const size_t recv_bytes = recv_result.value().size();
                if (recv_bytes == 0) {
                    m_owner->setRecvError(HttpError(kConnectionClose));
                    return true;
                }

                m_owner->m_session->getRingBuffer().produce(recv_bytes);
                m_owner->m_total_received += recv_bytes;
            }
        }
#endif

    private:
        bool prepareRecvWindow() {
            auto write_iovecs = borrowWriteIovecs(m_owner->m_session->getRingBuffer());
            if (write_iovecs.empty()) {
                return false;
            }
            this->m_plainBuffer = static_cast<char*>(write_iovecs[0].iov_base);
            this->m_plainLength = write_iovecs[0].iov_len;
            return this->m_plainLength > 0;
        }

        HttpSessionAwaitableImpl* m_owner;
    };

    HttpSessionAwaitableImpl(HttpSessionImpl<SocketType>& session, HttpRequest&& request)
        : galay::kernel::CustomAwaitable(session.getSocket().controller())
        , m_session(&session)
        , m_request(std::move(request))
        , m_send_buffer(m_request.toString())
        , m_total_received(0)
        , m_send_awaitable(this)
        , m_recv_awaitable(this)
        , m_result(std::nullopt)
    {
        addTask(IOEventType::SEND, &m_send_awaitable);
        addTask(IOEventType::RECV, &m_recv_awaitable);
    }

    bool await_ready() const noexcept {
        return false;
    }

    using galay::kernel::CustomAwaitable::await_suspend;

    std::expected<std::optional<HttpResponse>, HttpError> await_resume() {
        onCompleted();

        if (!m_result.has_value()) {
            auto& io_error = m_result.error();
            HttpErrorCode http_error_code = kTcpRecvError;
            if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kTimeout)) {
                http_error_code = kRequestTimeOut;
            } else if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
                http_error_code = kConnectionClose;
            }

            reset();
            return std::unexpected(HttpError(http_error_code, io_error.message()));
        }

        if (m_http_error.has_value()) {
            auto error = std::move(m_http_error.value());
            reset();
            return std::unexpected(std::move(error));
        }

        auto response = std::move(m_response);
        reset();
        return std::optional<HttpResponse>(std::move(response));
    }

private:
    bool parseResponseFromRingBuffer() {
        auto read_iovecs = borrowReadIovecs(m_session->getRingBuffer());
        if (read_iovecs.empty()) {
            return false;
        }

        std::vector<iovec> parse_iovecs;
        if (IoVecWindow::buildWindow(read_iovecs, parse_iovecs) == 0) {
            return false;
        }

        auto [error_code, consumed] = m_response.fromIOVec(parse_iovecs);
        if (consumed > 0) {
            m_session->getRingBuffer().consume(static_cast<size_t>(consumed));
        }

        if (error_code == kHeaderInComplete || error_code == kIncomplete) {
            if (m_total_received >= m_session->getReaderSetting().getMaxHeaderSize() && !m_response.isComplete()) {
                setRecvError(HttpError(kHeaderTooLarge));
                return true;
            }
            return false;
        }

        if (error_code != kNoError) {
            setRecvError(HttpError(error_code));
            return true;
        }

        return m_response.isComplete();
    }

    void setSslSendError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            m_http_error = HttpError(kConnectionClose, error.message());
            return;
        }
        m_http_error = HttpError(kSendError, error.message());
    }

    void setSslRecvError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            m_http_error = HttpError(kConnectionClose, error.message());
            return;
        }
        m_http_error = HttpError(kRecvError, error.message());
    }

    void setRecvError(HttpError&& error) {
        m_http_error = std::move(error);
    }

    void reset() {
        m_http_error.reset();
        m_send_buffer.clear();
        m_response.reset();
        m_result = std::nullopt;
    }

    HttpSessionImpl<SocketType>* m_session;
    HttpRequest m_request;
    HttpResponse m_response;
    std::string m_send_buffer;
    size_t m_send_offset = 0;
    bool m_send_completed = false;
    size_t m_total_received;
    char m_dummy_recv_buffer[1]{0};
    ProtocolSendAwaitable m_send_awaitable;
    ProtocolRecvAwaitable m_recv_awaitable;
    std::optional<HttpError> m_http_error;

public:
    std::expected<std::optional<HttpResponse>, galay::kernel::IOError> m_result;
};

// 类型别名 - HTTPS (SslSocket)
using HttpsSessionAwaitable = HttpSessionAwaitableImpl<galay::ssl::SslSocket>;
using HttpsSession = HttpSessionImpl<galay::ssl::SslSocket>;

} // namespace galay::http
#endif

#endif // GALAY_HTTP_SESSION_H
