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
#include "galay-ssl/SslSocket.h"
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
 * @brief HTTP请求读取等待体 - TcpSocket 版本（使用 readv）
 */
template<typename SocketType, bool IsSsl = is_ssl_socket_v<SocketType>>
class GetRequestAwaitableImpl;

// TcpSocket 特化版本（使用 readv）
template<typename SocketType>
class GetRequestAwaitableImpl<SocketType, false> : public galay::kernel::TimeoutSupport<GetRequestAwaitableImpl<SocketType, false>>
{
public:
    using ReadvAwaitableType = decltype(std::declval<SocketType>().readv(std::declval<std::vector<iovec>>()));

    GetRequestAwaitableImpl(RingBuffer& ring_buffer,
                           const HttpReaderSetting& setting,
                           HttpRequest& request,
                           SocketType& socket)
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_request(request)
        , m_socket(socket)
        , m_total_received(0)
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    template<typename Handle>
    auto await_suspend(Handle handle) {
        if (!m_readv_awaitable) {
            m_readv_awaitable.emplace(m_socket.readv(m_ring_buffer.getWriteIovecs()));
        }
        return m_readv_awaitable->await_suspend(handle);
    }

    std::expected<bool, HttpError> await_resume() {
        auto readv_result = m_readv_awaitable->await_resume();
        if (!readv_result) {
            if (galay::kernel::IOError::contains(readv_result.error().code(), galay::kernel::kDisconnectError)) {
                HTTP_LOG_DEBUG("connection closed by peer (disconnect error)");
                return std::unexpected(HttpError(kConnectionClose));
            }
            HTTP_LOG_DEBUG("readv failed: {}", readv_result.error().message());
            return std::unexpected(HttpError(kRecvError, readv_result.error().message()));
        }

        ssize_t bytes_read = readv_result.value();

        if (bytes_read == 0) {
            HTTP_LOG_DEBUG("connection closed by peer");
            return std::unexpected(HttpError(kConnectionClose));
        }

        m_ring_buffer.produce(bytes_read);
        m_total_received += bytes_read;

        auto read_iovecs = m_ring_buffer.getReadIovecs();
        if (read_iovecs.empty()) {
            return false;
        }

        auto [error_code, consumed] = m_request.fromIOVec(read_iovecs);

        if (consumed > 0) {
            m_ring_buffer.consume(consumed);
        }

        if (error_code == kHeaderInComplete || error_code == kIncomplete) {
            if (m_total_received >= m_setting.getMaxHeaderSize() && !m_request.isComplete()) {
                HTTP_LOG_DEBUG("header too large: received {} bytes, max: {}",
                            m_total_received, m_setting.getMaxHeaderSize());
                return std::unexpected(HttpError(kHeaderTooLarge));
            }
            return false;
        }

        if (error_code != kNoError) {
            HTTP_LOG_DEBUG("parse error: {}", static_cast<int>(error_code));
            return std::unexpected(HttpError(error_code));
        }

        if (m_request.isComplete()) {
            return true;
        }

        return false;
    }

private:
    RingBuffer& m_ring_buffer;
    const HttpReaderSetting& m_setting;
    HttpRequest& m_request;
    SocketType& m_socket;
    size_t m_total_received;
    std::optional<ReadvAwaitableType> m_readv_awaitable;

public:
    std::expected<bool, galay::kernel::IOError> m_result;
};

#ifdef GALAY_HTTP_SSL_ENABLED
// SslSocket 特化版本（使用 recv）
template<typename SocketType>
class GetRequestAwaitableImpl<SocketType, true> : public galay::kernel::TimeoutSupport<GetRequestAwaitableImpl<SocketType, true>>
{
public:
    using RecvAwaitableType = decltype(std::declval<SocketType>().recv(std::declval<char*>(), std::declval<size_t>()));

    GetRequestAwaitableImpl(RingBuffer& ring_buffer,
                           const HttpReaderSetting& setting,
                           HttpRequest& request,
                           SocketType& socket)
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_request(request)
        , m_socket(socket)
        , m_total_received(0)
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    template<typename Handle>
    auto await_suspend(Handle handle) {
        if (!m_recv_awaitable) {
            auto write_iovecs = m_ring_buffer.getWriteIovecs();
            if (!write_iovecs.empty()) {
                m_recv_awaitable.emplace(m_socket.recv(
                    static_cast<char*>(write_iovecs[0].iov_base),
                    write_iovecs[0].iov_len));
            }
        }
        return m_recv_awaitable->await_suspend(handle);
    }

    std::expected<bool, HttpError> await_resume() {
        auto recv_result = m_recv_awaitable->await_resume();
        m_recv_awaitable.reset();  // 重置 awaitable 以便下次重新创建

        if (!recv_result) {
            auto& error = recv_result.error();
            HTTP_LOG_DEBUG("SSL recv failed (request): code={}, ssl_error={}, message={}",
                          static_cast<int>(error.code()),
                          error.sslError(),
                          error.message());

            // 检查是否是正常的连接关闭
            if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
                HTTP_LOG_DEBUG("SSL connection closed by peer");
                return std::unexpected(HttpError(kConnectionClose));
            }

            // 检查是否是需要重试的情况 (SSL_ERROR_WANT_READ = 2, SSL_ERROR_WANT_WRITE = 3)
            if (error.sslError() == SSL_ERROR_WANT_READ || error.sslError() == SSL_ERROR_WANT_WRITE) {
                HTTP_LOG_DEBUG("SSL recv needs retry (want read/write)");
                return false;  // 返回 false 表示需要继续读取
            }

            return std::unexpected(HttpError(kRecvError, recv_result.error().message()));
        }

        ssize_t bytes_read = static_cast<ssize_t>(recv_result.value().size());
        HTTP_LOG_DEBUG("SSL recv succeeded (request): {} bytes", bytes_read);

        if (bytes_read == 0) {
            HTTP_LOG_DEBUG("SSL connection closed by peer (0 bytes)");
            return std::unexpected(HttpError(kConnectionClose));
        }

        m_ring_buffer.produce(bytes_read);
        m_total_received += bytes_read;

        auto read_iovecs = m_ring_buffer.getReadIovecs();
        if (read_iovecs.empty()) {
            return false;
        }

        auto [error_code, consumed] = m_request.fromIOVec(read_iovecs);

        if (consumed > 0) {
            m_ring_buffer.consume(consumed);
        }

        if (error_code == kHeaderInComplete || error_code == kIncomplete) {
            if (m_total_received >= m_setting.getMaxHeaderSize() && !m_request.isComplete()) {
                HTTP_LOG_DEBUG("header too large: received {} bytes, max: {}",
                            m_total_received, m_setting.getMaxHeaderSize());
                return std::unexpected(HttpError(kHeaderTooLarge));
            }
            return false;
        }

        if (error_code != kNoError) {
            HTTP_LOG_DEBUG("parse error: {}", static_cast<int>(error_code));
            return std::unexpected(HttpError(error_code));
        }

        if (m_request.isComplete()) {
            return true;
        }

        return false;
    }

private:
    RingBuffer& m_ring_buffer;
    const HttpReaderSetting& m_setting;
    HttpRequest& m_request;
    SocketType& m_socket;
    size_t m_total_received;
    std::optional<RecvAwaitableType> m_recv_awaitable;

public:
    std::expected<bool, galay::kernel::IOError> m_result;
};
#endif

/**
 * @brief HTTP响应读取等待体
 */
template<typename SocketType, bool IsSsl = is_ssl_socket_v<SocketType>>
class GetResponseAwaitableImpl;

// TcpSocket 特化版本
template<typename SocketType>
class GetResponseAwaitableImpl<SocketType, false> : public galay::kernel::TimeoutSupport<GetResponseAwaitableImpl<SocketType, false>>
{
public:
    using ReadvAwaitableType = decltype(std::declval<SocketType>().readv(std::declval<std::vector<iovec>>()));

    GetResponseAwaitableImpl(RingBuffer& ring_buffer,
                            const HttpReaderSetting& setting,
                            HttpResponse& response,
                            SocketType& socket)
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_response(response)
        , m_socket(socket)
        , m_total_received(0)
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    template<typename Handle>
    auto await_suspend(Handle handle) {
        if (!m_readv_awaitable) {
            m_readv_awaitable.emplace(m_socket.readv(m_ring_buffer.getWriteIovecs()));
        }
        return m_readv_awaitable->await_suspend(handle);
    }

    std::expected<bool, HttpError> await_resume() {
        auto readv_result = m_readv_awaitable->await_resume();
        if (!readv_result) {
            if (galay::kernel::IOError::contains(readv_result.error().code(), galay::kernel::kDisconnectError)) {
                HTTP_LOG_DEBUG("connection closed by peer (disconnect error)");
                return std::unexpected(HttpError(kConnectionClose));
            }
            HTTP_LOG_DEBUG("readv failed: {}", readv_result.error().message());
            return std::unexpected(HttpError(kRecvError, readv_result.error().message()));
        }

        ssize_t bytes_read = readv_result.value();

        if (bytes_read == 0) {
            HTTP_LOG_DEBUG("connection closed by peer");
            return std::unexpected(HttpError(kConnectionClose));
        }

        m_ring_buffer.produce(bytes_read);
        m_total_received += bytes_read;

        auto read_iovecs = m_ring_buffer.getReadIovecs();
        if (read_iovecs.empty()) {
            return false;
        }

        auto [error_code, consumed] = m_response.fromIOVec(read_iovecs);

        if (consumed > 0) {
            m_ring_buffer.consume(consumed);
        }

        if (error_code == kHeaderInComplete || error_code == kIncomplete) {
            if (m_total_received >= m_setting.getMaxHeaderSize() && !m_response.isComplete()) {
                HTTP_LOG_DEBUG("header too large: received {} bytes, max: {}",
                            m_total_received, m_setting.getMaxHeaderSize());
                return std::unexpected(HttpError(kHeaderTooLarge));
            }
            return false;
        }

        if (error_code != kNoError) {
            HTTP_LOG_DEBUG("parse error: {}", static_cast<int>(error_code));
            return std::unexpected(HttpError(error_code));
        }

        if (m_response.isComplete()) {
            return true;
        }

        return false;
    }

private:
    RingBuffer& m_ring_buffer;
    const HttpReaderSetting& m_setting;
    HttpResponse& m_response;
    SocketType& m_socket;
    size_t m_total_received;
    std::optional<ReadvAwaitableType> m_readv_awaitable;

public:
    std::expected<bool, galay::kernel::IOError> m_result;
};

#ifdef GALAY_HTTP_SSL_ENABLED
// SslSocket 特化版本
template<typename SocketType>
class GetResponseAwaitableImpl<SocketType, true> : public galay::kernel::TimeoutSupport<GetResponseAwaitableImpl<SocketType, true>>
{
public:
    using RecvAwaitableType = decltype(std::declval<SocketType>().recv(std::declval<char*>(), std::declval<size_t>()));

    GetResponseAwaitableImpl(RingBuffer& ring_buffer,
                            const HttpReaderSetting& setting,
                            HttpResponse& response,
                            SocketType& socket)
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_response(response)
        , m_socket(socket)
        , m_total_received(0)
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    template<typename Handle>
    auto await_suspend(Handle handle) {
        if (!m_recv_awaitable) {
            auto write_iovecs = m_ring_buffer.getWriteIovecs();
            if (!write_iovecs.empty()) {
                m_recv_awaitable.emplace(m_socket.recv(
                    static_cast<char*>(write_iovecs[0].iov_base),
                    write_iovecs[0].iov_len));
            }
        }
        return m_recv_awaitable->await_suspend(handle);
    }

    std::expected<bool, HttpError> await_resume() {
        auto recv_result = m_recv_awaitable->await_resume();
        m_recv_awaitable.reset();  // 重置 awaitable 以便下次重新创建

        if (!recv_result) {
            auto& error = recv_result.error();
            HTTP_LOG_DEBUG("SSL recv failed: code={}, ssl_error={}, message={}",
                          static_cast<int>(error.code()),
                          error.sslError(),
                          error.message());

            // 检查是否是正常的连接关闭
            if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
                HTTP_LOG_DEBUG("SSL connection closed by peer");
                return std::unexpected(HttpError(kConnectionClose));
            }

            // 检查是否是需要重试的情况 (SSL_ERROR_WANT_READ = 2, SSL_ERROR_WANT_WRITE = 3)
            if (error.sslError() == SSL_ERROR_WANT_READ || error.sslError() == SSL_ERROR_WANT_WRITE) {
                HTTP_LOG_DEBUG("SSL recv needs retry (want read/write)");
                return false;  // 返回 false 表示需要继续读取
            }

            return std::unexpected(HttpError(kRecvError, recv_result.error().message()));
        }

        ssize_t bytes_read = static_cast<ssize_t>(recv_result.value().size());
        HTTP_LOG_DEBUG("SSL recv succeeded: {} bytes", bytes_read);

        if (bytes_read == 0) {
            HTTP_LOG_DEBUG("SSL connection closed by peer (0 bytes)");
            return std::unexpected(HttpError(kConnectionClose));
        }

        m_ring_buffer.produce(bytes_read);
        m_total_received += bytes_read;

        auto read_iovecs = m_ring_buffer.getReadIovecs();
        if (read_iovecs.empty()) {
            return false;
        }

        auto [error_code, consumed] = m_response.fromIOVec(read_iovecs);

        if (consumed > 0) {
            m_ring_buffer.consume(consumed);
        }

        if (error_code == kHeaderInComplete || error_code == kIncomplete) {
            if (m_total_received >= m_setting.getMaxHeaderSize() && !m_response.isComplete()) {
                HTTP_LOG_DEBUG("header too large: received {} bytes, max: {}",
                            m_total_received, m_setting.getMaxHeaderSize());
                return std::unexpected(HttpError(kHeaderTooLarge));
            }
            return false;
        }

        if (error_code != kNoError) {
            HTTP_LOG_DEBUG("parse error: {}", static_cast<int>(error_code));
            return std::unexpected(HttpError(error_code));
        }

        if (m_response.isComplete()) {
            return true;
        }

        return false;
    }

private:
    RingBuffer& m_ring_buffer;
    const HttpReaderSetting& m_setting;
    HttpResponse& m_response;
    SocketType& m_socket;
    size_t m_total_received;
    std::optional<RecvAwaitableType> m_recv_awaitable;

public:
    std::expected<bool, galay::kernel::IOError> m_result;
};
#endif

/**
 * @brief HTTP Chunk读取等待体
 */
template<typename SocketType, bool IsSsl = is_ssl_socket_v<SocketType>>
class GetChunkAwaitableImpl;

// TcpSocket 特化版本
template<typename SocketType>
class GetChunkAwaitableImpl<SocketType, false> : public galay::kernel::TimeoutSupport<GetChunkAwaitableImpl<SocketType, false>>
{
public:
    using ReadvAwaitableType = decltype(std::declval<SocketType>().readv(std::declval<std::vector<iovec>>()));

    GetChunkAwaitableImpl(RingBuffer& ring_buffer,
                         const HttpReaderSetting& setting,
                         std::string& chunk_data,
                         SocketType& socket)
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_chunk_data(chunk_data)
        , m_socket(socket)
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    template<typename Handle>
    auto await_suspend(Handle handle) {
        if (!m_readv_awaitable) {
            m_readv_awaitable.emplace(m_socket.readv(m_ring_buffer.getWriteIovecs()));
        }
        return m_readv_awaitable->await_suspend(handle);
    }

    std::expected<bool, HttpError> await_resume() {
        auto readv_result = m_readv_awaitable->await_resume();
        if (!readv_result) {
            if (galay::kernel::IOError::contains(readv_result.error().code(), galay::kernel::kDisconnectError)) {
                HTTP_LOG_DEBUG("connection closed by peer (disconnect error)");
                return std::unexpected(HttpError(kConnectionClose));
            }
            HTTP_LOG_DEBUG("readv failed: {}", readv_result.error().message());
            return std::unexpected(HttpError(kRecvError, readv_result.error().message()));
        }

        ssize_t bytes_read = readv_result.value();

        if (bytes_read == 0) {
            HTTP_LOG_DEBUG("connection closed by peer");
            return std::unexpected(HttpError(kConnectionClose));
        }

        m_ring_buffer.produce(bytes_read);

        auto read_iovecs = m_ring_buffer.getReadIovecs();
        if (read_iovecs.empty()) {
            return false;
        }

        auto result = Chunk::fromIOVec(read_iovecs, m_chunk_data);

        if (!result) {
            auto& error = result.error();
            if (error.code() == kIncomplete) {
                return false;
            }
            HTTP_LOG_DEBUG("chunk parse error: {}", error.message());
            return std::unexpected(error);
        }

        auto [is_last, consumed] = result.value();
        m_ring_buffer.consume(consumed);

        if (is_last) {
            return true;
        }

        return false;
    }

private:
    RingBuffer& m_ring_buffer;
    const HttpReaderSetting& m_setting;
    std::string& m_chunk_data;
    SocketType& m_socket;
    std::optional<ReadvAwaitableType> m_readv_awaitable;

public:
    std::expected<bool, galay::kernel::IOError> m_result;
};

#ifdef GALAY_HTTP_SSL_ENABLED
// SslSocket 特化版本
template<typename SocketType>
class GetChunkAwaitableImpl<SocketType, true> : public galay::kernel::TimeoutSupport<GetChunkAwaitableImpl<SocketType, true>>
{
public:
    using RecvAwaitableType = decltype(std::declval<SocketType>().recv(std::declval<char*>(), std::declval<size_t>()));

    GetChunkAwaitableImpl(RingBuffer& ring_buffer,
                         const HttpReaderSetting& setting,
                         std::string& chunk_data,
                         SocketType& socket)
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_chunk_data(chunk_data)
        , m_socket(socket)
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    template<typename Handle>
    auto await_suspend(Handle handle) {
        if (!m_recv_awaitable) {
            auto write_iovecs = m_ring_buffer.getWriteIovecs();
            if (!write_iovecs.empty()) {
                m_recv_awaitable.emplace(m_socket.recv(
                    static_cast<char*>(write_iovecs[0].iov_base),
                    write_iovecs[0].iov_len));
            }
        }
        return m_recv_awaitable->await_suspend(handle);
    }

    std::expected<bool, HttpError> await_resume() {
        auto recv_result = m_recv_awaitable->await_resume();
        if (!recv_result) {
            HTTP_LOG_DEBUG("recv failed: {}", recv_result.error().message());
            return std::unexpected(HttpError(kRecvError, recv_result.error().message()));
        }

        ssize_t bytes_read = static_cast<ssize_t>(recv_result.value().size());

        if (bytes_read == 0) {
            HTTP_LOG_DEBUG("connection closed by peer");
            return std::unexpected(HttpError(kConnectionClose));
        }

        m_ring_buffer.produce(bytes_read);

        auto read_iovecs = m_ring_buffer.getReadIovecs();
        if (read_iovecs.empty()) {
            return false;
        }

        auto result = Chunk::fromIOVec(read_iovecs, m_chunk_data);

        if (!result) {
            auto& error = result.error();
            if (error.code() == kIncomplete) {
                return false;
            }
            HTTP_LOG_DEBUG("chunk parse error: {}", error.message());
            return std::unexpected(error);
        }

        auto [is_last, consumed] = result.value();
        m_ring_buffer.consume(consumed);

        if (is_last) {
            return true;
        }

        return false;
    }

private:
    RingBuffer& m_ring_buffer;
    const HttpReaderSetting& m_setting;
    std::string& m_chunk_data;
    SocketType& m_socket;
    std::optional<RecvAwaitableType> m_recv_awaitable;

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
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_socket(socket)
    {
    }

    GetRequestAwaitableImpl<SocketType> getRequest(HttpRequest& request) {
        return GetRequestAwaitableImpl<SocketType>(m_ring_buffer, m_setting, request, m_socket);
    }

    GetResponseAwaitableImpl<SocketType> getResponse(HttpResponse& response) {
        return GetResponseAwaitableImpl<SocketType>(m_ring_buffer, m_setting, response, m_socket);
    }

    GetChunkAwaitableImpl<SocketType> getChunk(std::string& chunk_data) {
        return GetChunkAwaitableImpl<SocketType>(m_ring_buffer, m_setting, chunk_data, m_socket);
    }

private:
    RingBuffer& m_ring_buffer;
    const HttpReaderSetting& m_setting;
    SocketType& m_socket;
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
