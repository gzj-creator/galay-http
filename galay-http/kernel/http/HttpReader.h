#ifndef GALAY_HTTP_READER_H
#define GALAY_HTTP_READER_H

#include "HttpReaderSetting.h"
#include "HttpLog.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/protoc/http/HttpError.h"
#include "galay-http/protoc/http/HttpChunk.h"
#include "galay-kernel/common/Buffer.h"
#include "galay-kernel/kernel/Awaitable.h"
#include "galay-kernel/kernel/Timeout.hpp"
#include "galay-kernel/async/TcpSocket.h"
#include <expected>
#include <coroutine>
#include <type_traits>
#include <variant>

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/async/SslSocket.h"
#include "galay-http/kernel/SslRecvCompatAwaitable.h"
#endif

namespace galay::http
{

using namespace galay::kernel;
using namespace galay::async;

// 类型特征：检测是否是 SslSocket
template<typename T>
struct is_ssl_socket : std::false_type {};

#ifdef GALAY_HTTP_SSL_ENABLED
template<>
struct is_ssl_socket<galay::ssl::SslSocket> : std::true_type {};
#endif

template<typename T>
inline constexpr bool is_ssl_socket_v = is_ssl_socket<T>::value;

// 前向声明
template<typename SocketType>
class HttpReaderImpl;

/**
 * @brief HTTP请求读取等待体 - TcpSocket 版本
 */
template<typename SocketType, bool IsSsl = is_ssl_socket_v<SocketType>>
class GetRequestAwaitableImpl;

// TcpSocket 特化版本（CustomAwaitable + RecvAwaitable）
template<typename SocketType>
class GetRequestAwaitableImpl<SocketType, false>
    : public galay::kernel::CustomAwaitable
    , public galay::kernel::TimeoutSupport<GetRequestAwaitableImpl<SocketType, false>>
{
public:
    class ProtocolRecvAwaitable : public galay::kernel::RecvAwaitable
    {
    public:
        explicit ProtocolRecvAwaitable(GetRequestAwaitableImpl* owner)
            : galay::kernel::RecvAwaitable(owner->m_socket->controller(), nullptr, 0)
            , m_owner(owner)
        {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle) override {
            if (m_owner->parseRequestFromRingBuffer()) {
                return true;
            }

            if (cqe == nullptr) {
                if (!prepareRecvWindow()) {
                    m_owner->setParseError(HttpError(kHeaderTooLarge));
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

            size_t recv_bytes = result.value().size();
            if (recv_bytes == 0) {
                m_owner->setParseError(HttpError(kConnectionClose));
                return true;
            }

            m_owner->m_ring_buffer->produce(recv_bytes);
            m_owner->m_total_received += recv_bytes;

            if (m_owner->parseRequestFromRingBuffer()) {
                return true;
            }

            if (!prepareRecvWindow()) {
                m_owner->setParseError(HttpError(kHeaderTooLarge));
                return true;
            }
            return false;
        }
#else
        bool handleComplete(GHandle handle) override {
            if (m_owner->parseRequestFromRingBuffer()) {
                return true;
            }

            while (true) {
                if (!prepareRecvWindow()) {
                    m_owner->setParseError(HttpError(kHeaderTooLarge));
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

                size_t recv_bytes = result.value().size();
                if (recv_bytes == 0) {
                    m_owner->setParseError(HttpError(kConnectionClose));
                    return true;
                }

                m_owner->m_ring_buffer->produce(recv_bytes);
                m_owner->m_total_received += recv_bytes;

                if (m_owner->parseRequestFromRingBuffer()) {
                    return true;
                }
            }
        }
#endif

    private:
        bool prepareRecvWindow() {
            auto write_iovecs = m_owner->m_ring_buffer->getWriteIovecs();
            if (write_iovecs.empty()) {
                return false;
            }

            m_buffer = static_cast<char*>(write_iovecs[0].iov_base);
            m_length = write_iovecs[0].iov_len;
            return m_length > 0;
        }

        GetRequestAwaitableImpl* m_owner;
    };

    GetRequestAwaitableImpl(RingBuffer& ring_buffer,
                           const HttpReaderSetting& setting,
                           HttpRequest& request,
                           SocketType& socket)
        : galay::kernel::CustomAwaitable(socket.controller())
        , m_ring_buffer(&ring_buffer)
        , m_setting(&setting)
        , m_request(&request)
        , m_socket(&socket)
        , m_total_received(0)
        , m_recv_awaitable(this)
        , m_result(true)
    {
        addTask(IOEventType::RECV, &m_recv_awaitable);
    }

    bool await_ready() const noexcept {
        return false;
    }

    using galay::kernel::CustomAwaitable::await_suspend;

    std::expected<bool, HttpError> await_resume() {
        onCompleted();

        if (!m_result.has_value()) {
            auto& io_error = m_result.error();
            if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
                HTTP_LOG_DEBUG("[conn] [closed]");
                return std::unexpected(HttpError(kConnectionClose));
            }
            HTTP_LOG_DEBUG("[recv] [fail] [{}]", io_error.message());
            return std::unexpected(HttpError(kRecvError, io_error.message()));
        }

        if (m_http_error.has_value()) {
            return std::unexpected(std::move(*m_http_error));
        }

        return true;
    }

private:
    bool parseRequestFromRingBuffer() {
        auto read_iovecs = m_ring_buffer->getReadIovecs();
        if (read_iovecs.empty()) {
            return false;
        }

        auto [error_code, consumed] = m_request->fromIOVec(read_iovecs);
        if (consumed > 0) {
            m_ring_buffer->consume(consumed);
        }

        if (error_code == kHeaderInComplete || error_code == kIncomplete) {
            if (m_total_received >= m_setting->getMaxHeaderSize() && !m_request->isComplete()) {
                HTTP_LOG_DEBUG("[header] [too-large] [recv={}] [max={}]",
                               m_total_received, m_setting->getMaxHeaderSize());
                setParseError(HttpError(kHeaderTooLarge));
                return true;
            }
            return false;
        }

        if (error_code != kNoError) {
            HTTP_LOG_DEBUG("[parse] [error] [{}]", static_cast<int>(error_code));
            setParseError(HttpError(error_code));
            return true;
        }

        if (!m_request->isComplete()) {
            return false;
        }

        auto& header = m_request->header();
        std::string host = header.headerPairs().getValue("Host");
        HTTP_LOG_INFO("[{}] [{}] [{}]",
                      httpMethodToString(header.method()),
                      header.uri(),
                      host.empty() ? "-" : host);
        return true;
    }

    void setRecvError(const galay::kernel::IOError& io_error) {
        if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
            m_http_error = HttpError(kConnectionClose, io_error.message());
            return;
        }
        m_http_error = HttpError(kRecvError, io_error.message());
    }

    void setParseError(HttpError&& error) {
        m_http_error = std::move(error);
    }

    RingBuffer* m_ring_buffer;
    const HttpReaderSetting* m_setting;
    HttpRequest* m_request;
    SocketType* m_socket;
    size_t m_total_received;
    ProtocolRecvAwaitable m_recv_awaitable;
    std::optional<HttpError> m_http_error;

public:
    std::expected<bool, galay::kernel::IOError> m_result;
};

#ifdef GALAY_HTTP_SSL_ENABLED
// SslSocket 特化版本（CustomAwaitable + SslRecvAwaitable）
template<typename SocketType>
class GetRequestAwaitableImpl<SocketType, true>
    : public galay::kernel::CustomAwaitable
    , public galay::kernel::TimeoutSupport<GetRequestAwaitableImpl<SocketType, true>>
{
public:
    using RecvAwaitableType = galay::http::SslRecvCompatAwaitable;

    class ProtocolRecvAwaitable : public RecvAwaitableType
    {
    public:
        explicit ProtocolRecvAwaitable(GetRequestAwaitableImpl* owner)
            : RecvAwaitableType(owner->m_socket->recv(owner->m_dummy_recv_buffer, sizeof(owner->m_dummy_recv_buffer)))
            , m_owner(owner)
        {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override {
            if (m_owner->parseRequestFromRingBuffer()) {
                return true;
            }

            if (!prepareRecvWindow()) {
                m_owner->setParseError(HttpError(kHeaderTooLarge));
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
                m_owner->setParseError(HttpError(kConnectionClose));
                return true;
            }

            m_owner->m_ring_buffer->produce(recv_bytes);
            m_owner->m_total_received += recv_bytes;
            return m_owner->parseRequestFromRingBuffer();
        }
#else
        bool handleComplete(GHandle handle) override {
            if (m_owner->parseRequestFromRingBuffer()) {
                return true;
            }

            while (true) {
                if (!prepareRecvWindow()) {
                    m_owner->setParseError(HttpError(kHeaderTooLarge));
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
                    m_owner->setParseError(HttpError(kConnectionClose));
                    return true;
                }

                m_owner->m_ring_buffer->produce(recv_bytes);
                m_owner->m_total_received += recv_bytes;
                if (m_owner->parseRequestFromRingBuffer()) {
                    return true;
                }
            }
        }
#endif

    private:
        bool prepareRecvWindow() {
            auto write_iovecs = m_owner->m_ring_buffer->getWriteIovecs();
            if (write_iovecs.empty()) {
                return false;
            }
            this->m_plainBuffer = static_cast<char*>(write_iovecs[0].iov_base);
            this->m_plainLength = write_iovecs[0].iov_len;
            return this->m_plainLength > 0;
        }

        GetRequestAwaitableImpl* m_owner;
    };

    GetRequestAwaitableImpl(RingBuffer& ring_buffer,
                           const HttpReaderSetting& setting,
                           HttpRequest& request,
                           SocketType& socket)
        : galay::kernel::CustomAwaitable(socket.controller())
        , m_ring_buffer(&ring_buffer)
        , m_setting(&setting)
        , m_request(&request)
        , m_socket(&socket)
        , m_total_received(0)
        , m_recv_awaitable(this)
        , m_result(true)
    {
        addTask(IOEventType::RECV, &m_recv_awaitable);
    }

    bool await_ready() const noexcept {
        return false;
    }

    using galay::kernel::CustomAwaitable::await_suspend;

    std::expected<bool, HttpError> await_resume() {
        onCompleted();

        if (!m_result.has_value()) {
            const auto& io_error = m_result.error();
            if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
                return std::unexpected(HttpError(kConnectionClose));
            }
            return std::unexpected(HttpError(kRecvError, io_error.message()));
        }

        if (m_http_error.has_value()) {
            return std::unexpected(std::move(*m_http_error));
        }

        return true;
    }

private:
    bool parseRequestFromRingBuffer() {
        auto read_iovecs = m_ring_buffer->getReadIovecs();
        if (read_iovecs.empty()) {
            return false;
        }

        auto [error_code, consumed] = m_request->fromIOVec(read_iovecs);
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
        std::string host = header.headerPairs().getValue("Host");
        HTTP_LOG_INFO("[{}] [{}] [{}]",
                      httpMethodToString(header.method()),
                      header.uri(),
                      host.empty() ? "-" : host);
        return true;
    }

    void setSslRecvError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            m_http_error = HttpError(kConnectionClose);
            return;
        }
        m_http_error = HttpError(kRecvError, error.message());
    }

    void setParseError(HttpError&& error) {
        m_http_error = std::move(error);
    }

    RingBuffer* m_ring_buffer;
    const HttpReaderSetting* m_setting;
    HttpRequest* m_request;
    SocketType* m_socket;
    size_t m_total_received;
    char m_dummy_recv_buffer[1];
    ProtocolRecvAwaitable m_recv_awaitable;
    std::optional<HttpError> m_http_error;

public:
    std::expected<bool, galay::kernel::IOError> m_result;
};
#endif

/**
 * @brief HTTP响应读取等待体
 */
template<typename SocketType, bool IsSsl = is_ssl_socket_v<SocketType>>
class GetResponseAwaitableImpl;

// TcpSocket 特化版本（CustomAwaitable + RecvAwaitable）
template<typename SocketType>
class GetResponseAwaitableImpl<SocketType, false>
    : public galay::kernel::CustomAwaitable
    , public galay::kernel::TimeoutSupport<GetResponseAwaitableImpl<SocketType, false>>
{
public:
    class ProtocolRecvAwaitable : public galay::kernel::RecvAwaitable
    {
    public:
        explicit ProtocolRecvAwaitable(GetResponseAwaitableImpl* owner)
            : galay::kernel::RecvAwaitable(owner->m_socket->controller(), nullptr, 0)
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
                    m_owner->setParseError(HttpError(kHeaderTooLarge));
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

            size_t recv_bytes = result.value().size();
            if (recv_bytes == 0) {
                m_owner->setParseError(HttpError(kConnectionClose));
                return true;
            }

            m_owner->m_ring_buffer->produce(recv_bytes);
            m_owner->m_total_received += recv_bytes;

            if (m_owner->parseResponseFromRingBuffer()) {
                return true;
            }

            if (!prepareRecvWindow()) {
                m_owner->setParseError(HttpError(kHeaderTooLarge));
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
                    m_owner->setParseError(HttpError(kHeaderTooLarge));
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

                size_t recv_bytes = result.value().size();
                if (recv_bytes == 0) {
                    m_owner->setParseError(HttpError(kConnectionClose));
                    return true;
                }

                m_owner->m_ring_buffer->produce(recv_bytes);
                m_owner->m_total_received += recv_bytes;

                if (m_owner->parseResponseFromRingBuffer()) {
                    return true;
                }
            }
        }
#endif

    private:
        bool prepareRecvWindow() {
            auto write_iovecs = m_owner->m_ring_buffer->getWriteIovecs();
            if (write_iovecs.empty()) {
                return false;
            }

            m_buffer = static_cast<char*>(write_iovecs[0].iov_base);
            m_length = write_iovecs[0].iov_len;
            return m_length > 0;
        }

        GetResponseAwaitableImpl* m_owner;
    };

    GetResponseAwaitableImpl(RingBuffer& ring_buffer,
                            const HttpReaderSetting& setting,
                            HttpResponse& response,
                            SocketType& socket)
        : galay::kernel::CustomAwaitable(socket.controller())
        , m_ring_buffer(&ring_buffer)
        , m_setting(&setting)
        , m_response(&response)
        , m_socket(&socket)
        , m_total_received(0)
        , m_recv_awaitable(this)
        , m_result(true)
    {
        addTask(IOEventType::RECV, &m_recv_awaitable);
    }

    bool await_ready() const noexcept {
        return false;
    }

    using galay::kernel::CustomAwaitable::await_suspend;

    std::expected<bool, HttpError> await_resume() {
        onCompleted();

        if (!m_result.has_value()) {
            auto& io_error = m_result.error();
            if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
                HTTP_LOG_DEBUG("[conn] [closed]");
                return std::unexpected(HttpError(kConnectionClose));
            }
            HTTP_LOG_DEBUG("[recv] [fail] [{}]", io_error.message());
            return std::unexpected(HttpError(kRecvError, io_error.message()));
        }

        if (m_http_error.has_value()) {
            return std::unexpected(std::move(*m_http_error));
        }

        return true;
    }

private:
    bool parseResponseFromRingBuffer() {
        auto read_iovecs = m_ring_buffer->getReadIovecs();
        if (read_iovecs.empty()) {
            return false;
        }

        auto [error_code, consumed] = m_response->fromIOVec(read_iovecs);
        if (consumed > 0) {
            m_ring_buffer->consume(consumed);
        }

        if (error_code == kHeaderInComplete || error_code == kIncomplete) {
            if (m_total_received >= m_setting->getMaxHeaderSize() && !m_response->isComplete()) {
                HTTP_LOG_DEBUG("[header] [too-large] [recv={}] [max={}]",
                               m_total_received, m_setting->getMaxHeaderSize());
                setParseError(HttpError(kHeaderTooLarge));
                return true;
            }
            return false;
        }

        if (error_code != kNoError) {
            HTTP_LOG_DEBUG("[parse] [error] [{}]", static_cast<int>(error_code));
            setParseError(HttpError(error_code));
            return true;
        }

        return m_response->isComplete();
    }

    void setRecvError(const galay::kernel::IOError& io_error) {
        if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
            m_http_error = HttpError(kConnectionClose, io_error.message());
            return;
        }
        m_http_error = HttpError(kRecvError, io_error.message());
    }

    void setParseError(HttpError&& error) {
        m_http_error = std::move(error);
    }

    RingBuffer* m_ring_buffer;
    const HttpReaderSetting* m_setting;
    HttpResponse* m_response;
    SocketType* m_socket;
    size_t m_total_received;
    ProtocolRecvAwaitable m_recv_awaitable;
    std::optional<HttpError> m_http_error;

public:
    std::expected<bool, galay::kernel::IOError> m_result;
};

#ifdef GALAY_HTTP_SSL_ENABLED
// SslSocket 特化版本（CustomAwaitable + SslRecvAwaitable）
template<typename SocketType>
class GetResponseAwaitableImpl<SocketType, true>
    : public galay::kernel::CustomAwaitable
    , public galay::kernel::TimeoutSupport<GetResponseAwaitableImpl<SocketType, true>>
{
public:
    using RecvAwaitableType = galay::http::SslRecvCompatAwaitable;

    class ProtocolRecvAwaitable : public RecvAwaitableType
    {
    public:
        explicit ProtocolRecvAwaitable(GetResponseAwaitableImpl* owner)
            : RecvAwaitableType(owner->m_socket->recv(owner->m_dummy_recv_buffer, sizeof(owner->m_dummy_recv_buffer)))
            , m_owner(owner)
        {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override {
            if (m_owner->parseResponseFromRingBuffer()) {
                return true;
            }

            if (!prepareRecvWindow()) {
                m_owner->setParseError(HttpError(kHeaderTooLarge));
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
                m_owner->setParseError(HttpError(kConnectionClose));
                return true;
            }

            m_owner->m_ring_buffer->produce(recv_bytes);
            m_owner->m_total_received += recv_bytes;
            return m_owner->parseResponseFromRingBuffer();
        }
#else
        bool handleComplete(GHandle handle) override {
            if (m_owner->parseResponseFromRingBuffer()) {
                return true;
            }

            while (true) {
                if (!prepareRecvWindow()) {
                    m_owner->setParseError(HttpError(kHeaderTooLarge));
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
                    m_owner->setParseError(HttpError(kConnectionClose));
                    return true;
                }

                m_owner->m_ring_buffer->produce(recv_bytes);
                m_owner->m_total_received += recv_bytes;
                if (m_owner->parseResponseFromRingBuffer()) {
                    return true;
                }
            }
        }
#endif

    private:
        bool prepareRecvWindow() {
            auto write_iovecs = m_owner->m_ring_buffer->getWriteIovecs();
            if (write_iovecs.empty()) {
                return false;
            }
            this->m_plainBuffer = static_cast<char*>(write_iovecs[0].iov_base);
            this->m_plainLength = write_iovecs[0].iov_len;
            return this->m_plainLength > 0;
        }

        GetResponseAwaitableImpl* m_owner;
    };

    GetResponseAwaitableImpl(RingBuffer& ring_buffer,
                            const HttpReaderSetting& setting,
                            HttpResponse& response,
                            SocketType& socket)
        : galay::kernel::CustomAwaitable(socket.controller())
        , m_ring_buffer(&ring_buffer)
        , m_setting(&setting)
        , m_response(&response)
        , m_socket(&socket)
        , m_total_received(0)
        , m_recv_awaitable(this)
        , m_result(true)
    {
        addTask(IOEventType::RECV, &m_recv_awaitable);
    }

    bool await_ready() const noexcept {
        return false;
    }

    using galay::kernel::CustomAwaitable::await_suspend;

    std::expected<bool, HttpError> await_resume() {
        onCompleted();

        if (!m_result.has_value()) {
            const auto& io_error = m_result.error();
            if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
                return std::unexpected(HttpError(kConnectionClose));
            }
            return std::unexpected(HttpError(kRecvError, io_error.message()));
        }

        if (m_http_error.has_value()) {
            return std::unexpected(std::move(*m_http_error));
        }

        return true;
    }

private:
    bool parseResponseFromRingBuffer() {
        auto read_iovecs = m_ring_buffer->getReadIovecs();
        if (read_iovecs.empty()) {
            return false;
        }

        auto [error_code, consumed] = m_response->fromIOVec(read_iovecs);
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

    void setSslRecvError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            m_http_error = HttpError(kConnectionClose);
            return;
        }
        m_http_error = HttpError(kRecvError, error.message());
    }

    void setParseError(HttpError&& error) {
        m_http_error = std::move(error);
    }

    RingBuffer* m_ring_buffer;
    const HttpReaderSetting* m_setting;
    HttpResponse* m_response;
    SocketType* m_socket;
    size_t m_total_received;
    char m_dummy_recv_buffer[1];
    ProtocolRecvAwaitable m_recv_awaitable;
    std::optional<HttpError> m_http_error;

public:
    std::expected<bool, galay::kernel::IOError> m_result;
};
#endif

/**
 * @brief HTTP Chunk读取等待体
 */
template<typename SocketType, bool IsSsl = is_ssl_socket_v<SocketType>>
class GetChunkAwaitableImpl;

// TcpSocket 特化版本（CustomAwaitable + RecvAwaitable）
template<typename SocketType>
class GetChunkAwaitableImpl<SocketType, false>
    : public galay::kernel::CustomAwaitable
    , public galay::kernel::TimeoutSupport<GetChunkAwaitableImpl<SocketType, false>>
{
public:
    class ProtocolRecvAwaitable : public galay::kernel::RecvAwaitable
    {
    public:
        explicit ProtocolRecvAwaitable(GetChunkAwaitableImpl* owner)
            : galay::kernel::RecvAwaitable(owner->m_socket->controller(), nullptr, 0)
            , m_owner(owner)
        {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle) override {
            if (m_owner->parseChunkFromRingBuffer()) {
                return true;
            }

            if (cqe == nullptr) {
                if (!prepareRecvWindow()) {
                    m_owner->setParseError(HttpError(kRecvError, "RingBuffer is full"));
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

            size_t recv_bytes = result.value().size();
            if (recv_bytes == 0) {
                m_owner->setParseError(HttpError(kConnectionClose));
                return true;
            }

            m_owner->m_ring_buffer->produce(recv_bytes);

            if (m_owner->parseChunkFromRingBuffer()) {
                return true;
            }

            if (!prepareRecvWindow()) {
                m_owner->setParseError(HttpError(kRecvError, "RingBuffer is full"));
                return true;
            }
            return false;
        }
#else
        bool handleComplete(GHandle handle) override {
            if (m_owner->parseChunkFromRingBuffer()) {
                return true;
            }

            while (true) {
                if (!prepareRecvWindow()) {
                    m_owner->setParseError(HttpError(kRecvError, "RingBuffer is full"));
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

                size_t recv_bytes = result.value().size();
                if (recv_bytes == 0) {
                    m_owner->setParseError(HttpError(kConnectionClose));
                    return true;
                }

                m_owner->m_ring_buffer->produce(recv_bytes);

                if (m_owner->parseChunkFromRingBuffer()) {
                    return true;
                }
            }
        }
#endif

    private:
        bool prepareRecvWindow() {
            auto write_iovecs = m_owner->m_ring_buffer->getWriteIovecs();
            if (write_iovecs.empty()) {
                return false;
            }

            m_buffer = static_cast<char*>(write_iovecs[0].iov_base);
            m_length = write_iovecs[0].iov_len;
            return m_length > 0;
        }

        GetChunkAwaitableImpl* m_owner;
    };

    GetChunkAwaitableImpl(RingBuffer& ring_buffer,
                         const HttpReaderSetting& setting,
                         std::string& chunk_data,
                         SocketType& socket)
        : galay::kernel::CustomAwaitable(socket.controller())
        , m_ring_buffer(&ring_buffer)
        , m_setting(&setting)
        , m_chunk_data(&chunk_data)
        , m_socket(&socket)
        , m_recv_awaitable(this)
        , m_result(true)
    {
        addTask(IOEventType::RECV, &m_recv_awaitable);
    }

    bool await_ready() const noexcept {
        return false;
    }

    using galay::kernel::CustomAwaitable::await_suspend;

    std::expected<bool, HttpError> await_resume() {
        onCompleted();

        if (!m_result.has_value()) {
            auto& io_error = m_result.error();
            if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
                HTTP_LOG_DEBUG("[conn] [closed]");
                return std::unexpected(HttpError(kConnectionClose));
            }
            HTTP_LOG_DEBUG("[recv] [fail] [{}]", io_error.message());
            return std::unexpected(HttpError(kRecvError, io_error.message()));
        }

        if (m_http_error.has_value()) {
            return std::unexpected(std::move(*m_http_error));
        }

        return true;
    }

private:
    bool parseChunkFromRingBuffer() {
        auto read_iovecs = m_ring_buffer->getReadIovecs();
        if (read_iovecs.empty()) {
            return false;
        }

        auto result = Chunk::fromIOVec(read_iovecs, *m_chunk_data);
        if (!result) {
            auto& error = result.error();
            if (error.code() == kIncomplete) {
                return false;
            }
            HTTP_LOG_DEBUG("[chunk] [parse-fail] [{}]", error.message());
            setParseError(HttpError(error.code(), error.message()));
            return true;
        }

        auto [is_last, consumed] = result.value();
        m_ring_buffer->consume(consumed);
        return is_last;
    }

    void setRecvError(const galay::kernel::IOError& io_error) {
        if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
            m_http_error = HttpError(kConnectionClose, io_error.message());
            return;
        }
        m_http_error = HttpError(kRecvError, io_error.message());
    }

    void setParseError(HttpError&& error) {
        m_http_error = std::move(error);
    }

    RingBuffer* m_ring_buffer;
    const HttpReaderSetting* m_setting;
    std::string* m_chunk_data;
    SocketType* m_socket;
    ProtocolRecvAwaitable m_recv_awaitable;
    std::optional<HttpError> m_http_error;

public:
    std::expected<bool, galay::kernel::IOError> m_result;
};

#ifdef GALAY_HTTP_SSL_ENABLED
// SslSocket 特化版本（CustomAwaitable + SslRecvAwaitable）
template<typename SocketType>
class GetChunkAwaitableImpl<SocketType, true>
    : public galay::kernel::CustomAwaitable
    , public galay::kernel::TimeoutSupport<GetChunkAwaitableImpl<SocketType, true>>
{
public:
    using RecvAwaitableType = galay::http::SslRecvCompatAwaitable;

    class ProtocolRecvAwaitable : public RecvAwaitableType
    {
    public:
        explicit ProtocolRecvAwaitable(GetChunkAwaitableImpl* owner)
            : RecvAwaitableType(owner->m_socket->recv(owner->m_dummy_recv_buffer, sizeof(owner->m_dummy_recv_buffer)))
            , m_owner(owner)
        {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override {
            if (m_owner->parseChunkFromRingBuffer()) {
                return true;
            }

            if (!prepareRecvWindow()) {
                m_owner->setParseError(HttpError(kRecvError, "RingBuffer is full"));
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
                m_owner->setParseError(HttpError(kConnectionClose));
                return true;
            }

            m_owner->m_ring_buffer->produce(recv_bytes);
            return m_owner->parseChunkFromRingBuffer();
        }
#else
        bool handleComplete(GHandle handle) override {
            if (m_owner->parseChunkFromRingBuffer()) {
                return true;
            }

            while (true) {
                if (!prepareRecvWindow()) {
                    m_owner->setParseError(HttpError(kRecvError, "RingBuffer is full"));
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
                    m_owner->setParseError(HttpError(kConnectionClose));
                    return true;
                }

                m_owner->m_ring_buffer->produce(recv_bytes);
                if (m_owner->parseChunkFromRingBuffer()) {
                    return true;
                }
            }
        }
#endif

    private:
        bool prepareRecvWindow() {
            auto write_iovecs = m_owner->m_ring_buffer->getWriteIovecs();
            if (write_iovecs.empty()) {
                return false;
            }
            this->m_plainBuffer = static_cast<char*>(write_iovecs[0].iov_base);
            this->m_plainLength = write_iovecs[0].iov_len;
            return this->m_plainLength > 0;
        }

        GetChunkAwaitableImpl* m_owner;
    };

    GetChunkAwaitableImpl(RingBuffer& ring_buffer,
                         const HttpReaderSetting& setting,
                         std::string& chunk_data,
                         SocketType& socket)
        : galay::kernel::CustomAwaitable(socket.controller())
        , m_ring_buffer(&ring_buffer)
        , m_setting(&setting)
        , m_chunk_data(&chunk_data)
        , m_socket(&socket)
        , m_recv_awaitable(this)
        , m_result(true)
    {
        addTask(IOEventType::RECV, &m_recv_awaitable);
    }

    bool await_ready() const noexcept {
        return false;
    }

    using galay::kernel::CustomAwaitable::await_suspend;

    std::expected<bool, HttpError> await_resume() {
        onCompleted();

        if (!m_result.has_value()) {
            const auto& io_error = m_result.error();
            if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
                return std::unexpected(HttpError(kConnectionClose));
            }
            return std::unexpected(HttpError(kRecvError, io_error.message()));
        }

        if (m_http_error.has_value()) {
            return std::unexpected(std::move(*m_http_error));
        }

        return true;
    }

private:
    bool parseChunkFromRingBuffer() {
        auto read_iovecs = m_ring_buffer->getReadIovecs();
        if (read_iovecs.empty()) {
            return false;
        }

        auto result = Chunk::fromIOVec(read_iovecs, *m_chunk_data);
        if (!result) {
            auto& error = result.error();
            if (error.code() == kIncomplete) {
                return false;
            }
            setParseError(HttpError(error.code(), error.message()));
            return true;
        }

        auto [is_last, consumed] = result.value();
        m_ring_buffer->consume(consumed);
        return is_last;
    }

    void setSslRecvError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            m_http_error = HttpError(kConnectionClose);
            return;
        }
        m_http_error = HttpError(kRecvError, error.message());
    }

    void setParseError(HttpError&& error) {
        m_http_error = std::move(error);
    }

    RingBuffer* m_ring_buffer;
    const HttpReaderSetting* m_setting;
    std::string* m_chunk_data;
    SocketType* m_socket;
    char m_dummy_recv_buffer[1];
    ProtocolRecvAwaitable m_recv_awaitable;
    std::optional<HttpError> m_http_error;

public:
    std::expected<bool, galay::kernel::IOError> m_result;
};
#endif

/**
 * @brief HTTP读取器模板类
 */
template<typename SocketType>
class HttpReaderImpl
{
public:
    HttpReaderImpl(RingBuffer& ring_buffer, const HttpReaderSetting& setting, SocketType& socket)
        : m_ring_buffer(&ring_buffer)
        , m_setting(setting)
        , m_socket(&socket)
    {
    }

    GetRequestAwaitableImpl<SocketType> getRequest(HttpRequest& request) {
        return GetRequestAwaitableImpl<SocketType>(*m_ring_buffer, m_setting, request, *m_socket);
    }

    GetResponseAwaitableImpl<SocketType> getResponse(HttpResponse& response) {
        return GetResponseAwaitableImpl<SocketType>(*m_ring_buffer, m_setting, response, *m_socket);
    }

    GetChunkAwaitableImpl<SocketType> getChunk(std::string& chunk_data) {
        return GetChunkAwaitableImpl<SocketType>(*m_ring_buffer, m_setting, chunk_data, *m_socket);
    }

private:
    RingBuffer* m_ring_buffer;
    HttpReaderSetting m_setting;
    SocketType* m_socket;
};

// 类型别名 - HTTP (TcpSocket)
using GetRequestAwaitable = GetRequestAwaitableImpl<TcpSocket>;
using GetResponseAwaitable = GetResponseAwaitableImpl<TcpSocket>;
using GetChunkAwaitable = GetChunkAwaitableImpl<TcpSocket>;
using HttpReader = HttpReaderImpl<TcpSocket>;

} // namespace galay::http

#ifdef GALAY_HTTP_SSL_ENABLED
namespace galay::http {
using GetRequestAwaitableSsl = GetRequestAwaitableImpl<galay::ssl::SslSocket>;
using GetResponseAwaitableSsl = GetResponseAwaitableImpl<galay::ssl::SslSocket>;
using GetChunkAwaitableSsl = GetChunkAwaitableImpl<galay::ssl::SslSocket>;
using HttpsReader = HttpReaderImpl<galay::ssl::SslSocket>;
} // namespace galay::http
#endif

#endif // GALAY_HTTP_READER_H
