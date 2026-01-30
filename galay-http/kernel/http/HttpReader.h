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

namespace galay::http
{

using namespace galay::kernel;
using namespace galay::async;

// 前向声明
template<typename SocketType>
class HttpReaderImpl;

/**
 * @brief HTTP请求读取等待体
 * @tparam SocketType Socket类型（TcpSocket 或 SslSocket）
 */
template<typename SocketType>
class GetRequestAwaitableImpl : public galay::kernel::TimeoutSupport<GetRequestAwaitableImpl<SocketType>>
{
public:
    using ReadvAwaitableType = decltype(std::declval<SocketType>().readv(std::declval<std::vector<iovec>>()));

    GetRequestAwaitableImpl(RingBuffer& ring_buffer,
                           const HttpReaderSetting& setting,
                           HttpRequest& request,
                           ReadvAwaitableType&& readv_awaitable)
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_request(request)
        , m_readv_awaitable(std::move(readv_awaitable))
        , m_total_received(0)
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    auto await_suspend(std::coroutine_handle<> handle) {
        return m_readv_awaitable.await_suspend(handle);
    }

    std::expected<bool, HttpError> await_resume() {
        auto readv_result = m_readv_awaitable.await_resume();
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
    ReadvAwaitableType m_readv_awaitable;
    size_t m_total_received;

public:
    std::expected<bool, galay::kernel::IOError> m_result;
};

/**
 * @brief HTTP响应读取等待体
 * @tparam SocketType Socket类型（TcpSocket 或 SslSocket）
 */
template<typename SocketType>
class GetResponseAwaitableImpl : public galay::kernel::TimeoutSupport<GetResponseAwaitableImpl<SocketType>>
{
public:
    using ReadvAwaitableType = decltype(std::declval<SocketType>().readv(std::declval<std::vector<iovec>>()));

    GetResponseAwaitableImpl(RingBuffer& ring_buffer,
                            const HttpReaderSetting& setting,
                            HttpResponse& response,
                            ReadvAwaitableType&& readv_awaitable)
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_response(response)
        , m_readv_awaitable(std::move(readv_awaitable))
        , m_total_received(0)
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    auto await_suspend(std::coroutine_handle<> handle) {
        return m_readv_awaitable.await_suspend(handle);
    }

    std::expected<bool, HttpError> await_resume() {
        auto readv_result = m_readv_awaitable.await_resume();
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
    ReadvAwaitableType m_readv_awaitable;
    size_t m_total_received;

public:
    std::expected<bool, galay::kernel::IOError> m_result;
};

/**
 * @brief HTTP Chunk读取等待体
 * @tparam SocketType Socket类型（TcpSocket 或 SslSocket）
 */
template<typename SocketType>
class GetChunkAwaitableImpl : public galay::kernel::TimeoutSupport<GetChunkAwaitableImpl<SocketType>>
{
public:
    using ReadvAwaitableType = decltype(std::declval<SocketType>().readv(std::declval<std::vector<iovec>>()));

    GetChunkAwaitableImpl(RingBuffer& ring_buffer,
                         const HttpReaderSetting& setting,
                         std::string& chunk_data,
                         ReadvAwaitableType&& readv_awaitable)
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_chunk_data(chunk_data)
        , m_readv_awaitable(std::move(readv_awaitable))
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    auto await_suspend(std::coroutine_handle<> handle) {
        return m_readv_awaitable.await_suspend(handle);
    }

    std::expected<bool, HttpError> await_resume() {
        auto readv_result = m_readv_awaitable.await_resume();
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
    ReadvAwaitableType m_readv_awaitable;

public:
    std::expected<bool, galay::kernel::IOError> m_result;
};

/**
 * @brief HTTP读取器模板类
 * @tparam SocketType Socket类型（TcpSocket 或 SslSocket）
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
        return GetRequestAwaitableImpl<SocketType>(m_ring_buffer, m_setting, request,
                              m_socket.readv(m_ring_buffer.getWriteIovecs()));
    }

    GetResponseAwaitableImpl<SocketType> getResponse(HttpResponse& response) {
        return GetResponseAwaitableImpl<SocketType>(m_ring_buffer, m_setting, response,
                                m_socket.readv(m_ring_buffer.getWriteIovecs()));
    }

    GetChunkAwaitableImpl<SocketType> getChunk(std::string& chunk_data) {
        return GetChunkAwaitableImpl<SocketType>(m_ring_buffer, m_setting, chunk_data,
                             m_socket.readv(m_ring_buffer.getWriteIovecs()));
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

#ifdef GALAY_HTTP_SSL_ENABLED
// 类型别名 - HTTPS (SslSocket)
#include "galay-socket/async/SslSocket.h"
using GetRequestAwaitableSsl = GetRequestAwaitableImpl<galay::async::SslSocket>;
using GetResponseAwaitableSsl = GetResponseAwaitableImpl<galay::async::SslSocket>;
using GetChunkAwaitableSsl = GetChunkAwaitableImpl<galay::async::SslSocket>;
using HttpsReader = HttpReaderImpl<galay::async::SslSocket>;
#endif

} // namespace galay::http

#endif // GALAY_HTTP_READER_H
