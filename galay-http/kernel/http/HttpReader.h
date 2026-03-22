#ifndef GALAY_HTTP_READER_H
#define GALAY_HTTP_READER_H

#include "HttpReaderSetting.h"
#include "HttpLog.h"
#include "galay-http/kernel/IoVecUtils.h"
#include "galay-http/protoc/http/HttpChunk.h"
#include "galay-http/protoc/http/HttpError.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/common/Buffer.h"
#include "galay-kernel/kernel/Awaitable.h"
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/async/SslAwaitableCore.h"
#include "galay-ssl/async/SslSocket.h"
#endif

namespace galay::http {

using namespace galay::async;
using namespace galay::kernel;

template<typename T>
struct is_ssl_socket : std::false_type {};

#ifdef GALAY_HTTP_SSL_ENABLED
template<>
struct is_ssl_socket<galay::ssl::SslSocket> : std::true_type {};
#endif

template<typename T>
inline constexpr bool is_ssl_socket_v = is_ssl_socket<T>::value;

namespace detail {

template<typename StateT>
struct HttpRingBufferTcpReadMachine {
    using result_type = typename StateT::ResultType;
    static constexpr auto kSequenceOwnerDomain = galay::kernel::SequenceOwnerDomain::Read;

    explicit HttpRingBufferTcpReadMachine(std::shared_ptr<StateT> state)
        : m_state(std::move(state)) {}

    MachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
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

    void onWrite(std::expected<size_t, IOError>) {}

    std::shared_ptr<StateT> m_state;
    std::optional<result_type> m_result;
};

#ifdef GALAY_HTTP_SSL_ENABLED
template<typename StateT>
struct HttpRingBufferSslReadMachine {
    using result_type = typename StateT::ResultType;
    static constexpr auto kSequenceOwnerDomain = galay::kernel::SequenceOwnerDomain::Read;

    explicit HttpRingBufferSslReadMachine(std::shared_ptr<StateT> state)
        : m_state(std::move(state)) {}

    galay::ssl::SslMachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
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

    void onSend(std::expected<size_t, galay::ssl::SslError>) {}

    void onShutdown(std::expected<void, galay::ssl::SslError>) {}

    std::shared_ptr<StateT> m_state;
    std::optional<result_type> m_result;
};
#endif

struct HttpRequestReadState {
    using ResultType = std::expected<bool, HttpError>;

    HttpRequestReadState(RingBuffer& ring_buffer,
                         const HttpReaderSetting& setting,
                         HttpRequest& request)
        : m_ring_buffer(&ring_buffer)
        , m_setting(&setting)
        , m_request(&request) {}

    bool parseFromRingBuffer() {
        auto read_iovecs = borrowReadIovecs(*m_ring_buffer);
        if (read_iovecs.empty()) {
            return false;
        }

        if (IoVecWindow::buildWindow(read_iovecs, m_parse_iovecs) == 0) {
            return false;
        }

        auto [error_code, consumed] = m_request->fromIOVec(m_parse_iovecs);
        if (consumed > 0) {
            m_ring_buffer->consume(consumed);
        }

        if (error_code == kHeaderInComplete || error_code == kIncomplete) {
            if (m_total_received >= m_setting->getMaxHeaderSize() && !m_request->isComplete()) {
                setParseError(HttpError(kHeaderTooLarge));
                return true;
            }
            return false;
        }

        if (error_code != kNoError) {
            setParseError(HttpError(error_code));
            return true;
        }

        if (!m_request->isComplete()) {
            return false;
        }

        auto& header = m_request->header();
        const std::string host = header.headerPairs().getValue("host");
        HTTP_LOG_INFO("[{}] [{}] [{}]",
                      httpMethodToString(header.method()),
                      header.uri(),
                      host.empty() ? "-" : host);
        return true;
    }

    bool prepareRecvWindow() {
        m_write_iovecs = borrowWriteIovecs(*m_ring_buffer);
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

    void setRecvError(const IOError& io_error) {
        if (IOError::contains(io_error.code(), kDisconnectError)) {
            HTTP_LOG_DEBUG("[conn] [closed]");
            m_http_error = HttpError(kConnectionClose);
            return;
        }
        HTTP_LOG_DEBUG("[recv] [fail] [{}]", io_error.message());
        m_http_error = HttpError(kRecvError, io_error.message());
    }

#ifdef GALAY_HTTP_SSL_ENABLED
    void setSslRecvError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            m_http_error = HttpError(kConnectionClose);
            return;
        }
        m_http_error = HttpError(kRecvError, error.message());
    }
#endif

    void onPeerClosed() { m_http_error = HttpError(kConnectionClose); }

    void onBytesReceived(size_t recv_bytes) {
        m_ring_buffer->produce(recv_bytes);
        m_total_received += recv_bytes;
    }

    void setParseError(HttpError&& error) { m_http_error = std::move(error); }

    ResultType takeResult() {
        if (m_http_error.has_value()) {
            return std::unexpected(std::move(*m_http_error));
        }
        return true;
    }

    RingBuffer* m_ring_buffer;
    const HttpReaderSetting* m_setting;
    HttpRequest* m_request;
    size_t m_total_received = 0;
    std::vector<iovec> m_parse_iovecs;
    BorrowedIovecs<2> m_write_iovecs;
    std::optional<HttpError> m_http_error;
};

struct HttpResponseReadState {
    using ResultType = std::expected<bool, HttpError>;

    HttpResponseReadState(RingBuffer& ring_buffer,
                          const HttpReaderSetting& setting,
                          HttpResponse& response)
        : m_ring_buffer(&ring_buffer)
        , m_setting(&setting)
        , m_response(&response) {}

    bool parseFromRingBuffer() {
        auto read_iovecs = borrowReadIovecs(*m_ring_buffer);
        if (read_iovecs.empty()) {
            return false;
        }

        if (IoVecWindow::buildWindow(read_iovecs, m_parse_iovecs) == 0) {
            return false;
        }

        auto [error_code, consumed] = m_response->fromIOVec(m_parse_iovecs);
        if (consumed > 0) {
            m_ring_buffer->consume(consumed);
        }

        if (error_code == kHeaderInComplete || error_code == kIncomplete) {
            if (m_total_received >= m_setting->getMaxHeaderSize() && !m_response->isComplete()) {
                setParseError(HttpError(kHeaderTooLarge));
                return true;
            }
            return false;
        }

        if (error_code != kNoError) {
            setParseError(HttpError(error_code));
            return true;
        }

        return m_response->isComplete();
    }

    bool prepareRecvWindow() {
        m_write_iovecs = borrowWriteIovecs(*m_ring_buffer);
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

    void setRecvError(const IOError& io_error) {
        if (IOError::contains(io_error.code(), kDisconnectError)) {
            HTTP_LOG_DEBUG("[conn] [closed]");
            m_http_error = HttpError(kConnectionClose);
            return;
        }
        HTTP_LOG_DEBUG("[recv] [fail] [{}]", io_error.message());
        m_http_error = HttpError(kRecvError, io_error.message());
    }

#ifdef GALAY_HTTP_SSL_ENABLED
    void setSslRecvError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            m_http_error = HttpError(kConnectionClose);
            return;
        }
        m_http_error = HttpError(kRecvError, error.message());
    }
#endif

    void onPeerClosed() { m_http_error = HttpError(kConnectionClose); }

    void onBytesReceived(size_t recv_bytes) {
        m_ring_buffer->produce(recv_bytes);
        m_total_received += recv_bytes;
    }

    void setParseError(HttpError&& error) { m_http_error = std::move(error); }

    ResultType takeResult() {
        if (m_http_error.has_value()) {
            return std::unexpected(std::move(*m_http_error));
        }
        return true;
    }

    RingBuffer* m_ring_buffer;
    const HttpReaderSetting* m_setting;
    HttpResponse* m_response;
    size_t m_total_received = 0;
    std::vector<iovec> m_parse_iovecs;
    BorrowedIovecs<2> m_write_iovecs;
    std::optional<HttpError> m_http_error;
};

struct HttpChunkReadState {
    using ResultType = std::expected<bool, HttpError>;

    HttpChunkReadState(RingBuffer& ring_buffer, std::string& chunk_data)
        : m_ring_buffer(&ring_buffer)
        , m_chunk_data(&chunk_data) {}

    bool parseFromRingBuffer() {
        auto read_iovecs = borrowReadIovecs(*m_ring_buffer);
        if (read_iovecs.empty()) {
            return false;
        }

        if (IoVecWindow::buildWindow(read_iovecs, m_parse_iovecs) == 0) {
            return false;
        }

        auto result = Chunk::fromIOVec(m_parse_iovecs, *m_chunk_data);
        if (!result) {
            const auto& error = result.error();
            if (error.code() == kIncomplete) {
                return false;
            }
            HTTP_LOG_DEBUG("[chunk] [parse-fail] [{}]", error.message());
            setParseError(HttpError(error.code(), error.message()));
            return true;
        }

        auto [is_last, consumed] = result.value();
        m_ring_buffer->consume(consumed);
        m_is_last = is_last;
        return is_last;
    }

    bool prepareRecvWindow() {
        m_write_iovecs = borrowWriteIovecs(*m_ring_buffer);
        if (m_write_iovecs.empty()) {
            setParseError(HttpError(kRecvError, "RingBuffer is full"));
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
            setParseError(HttpError(kRecvError, "RingBuffer is full"));
            return false;
        }
        return true;
    }

    const struct iovec* recvIovecsData() const { return m_write_iovecs.data(); }
    size_t recvIovecsCount() const { return m_write_iovecs.size(); }

    void setRecvError(const IOError& io_error) {
        if (IOError::contains(io_error.code(), kDisconnectError)) {
            HTTP_LOG_DEBUG("[conn] [closed]");
            m_http_error = HttpError(kConnectionClose);
            return;
        }
        HTTP_LOG_DEBUG("[recv] [fail] [{}]", io_error.message());
        m_http_error = HttpError(kRecvError, io_error.message());
    }

#ifdef GALAY_HTTP_SSL_ENABLED
    void setSslRecvError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            m_http_error = HttpError(kConnectionClose);
            return;
        }
        m_http_error = HttpError(kRecvError, error.message());
    }
#endif

    void onPeerClosed() { m_http_error = HttpError(kConnectionClose); }

    void onBytesReceived(size_t recv_bytes) { m_ring_buffer->produce(recv_bytes); }

    void setParseError(HttpError&& error) { m_http_error = std::move(error); }

    ResultType takeResult() {
        if (m_http_error.has_value()) {
            return std::unexpected(std::move(*m_http_error));
        }
        return m_is_last;
    }

    RingBuffer* m_ring_buffer;
    std::string* m_chunk_data;
    std::vector<iovec> m_parse_iovecs;
    BorrowedIovecs<2> m_write_iovecs;
    std::optional<HttpError> m_http_error;
    bool m_is_last = false;
};

template<typename SocketType, typename StateT>
auto buildReadOperation(SocketType& socket, std::shared_ptr<StateT> state) {
    using ResultType = typename StateT::ResultType;
    if constexpr (is_ssl_socket_v<SocketType>) {
#ifdef GALAY_HTTP_SSL_ENABLED
        return galay::ssl::SslAwaitableBuilder<ResultType>::fromStateMachine(
                   socket.controller(),
                   &socket,
                   HttpRingBufferSslReadMachine<StateT>(std::move(state)))
            .build();
#else
        static_assert(!sizeof(SocketType), "SSL support is disabled");
#endif
    } else {
        return AwaitableBuilder<ResultType>::fromStateMachine(
                   socket.controller(),
                   HttpRingBufferTcpReadMachine<StateT>(std::move(state)))
            .build();
    }
}

} // namespace detail

template<typename SocketType>
class HttpReaderImpl {
public:
    HttpReaderImpl(RingBuffer& ring_buffer, const HttpReaderSetting& setting, SocketType& socket)
        : m_ring_buffer(&ring_buffer)
        , m_setting(setting)
        , m_socket(&socket) {}

    auto getRequest(HttpRequest& request) {
        return detail::buildReadOperation(
            *m_socket,
            std::make_shared<detail::HttpRequestReadState>(*m_ring_buffer, m_setting, request));
    }

    auto getResponse(HttpResponse& response) {
        return detail::buildReadOperation(
            *m_socket,
            std::make_shared<detail::HttpResponseReadState>(*m_ring_buffer, m_setting, response));
    }

    auto getChunk(std::string& chunk_data) {
        return detail::buildReadOperation(
            *m_socket,
            std::make_shared<detail::HttpChunkReadState>(*m_ring_buffer, chunk_data));
    }

private:
    RingBuffer* m_ring_buffer;
    HttpReaderSetting m_setting;
    SocketType* m_socket;
};

using HttpReader = HttpReaderImpl<TcpSocket>;

} // namespace galay::http

#ifdef GALAY_HTTP_SSL_ENABLED
namespace galay::http {
using HttpsReader = HttpReaderImpl<galay::ssl::SslSocket>;
} // namespace galay::http
#endif

#endif // GALAY_HTTP_READER_H
