#ifndef GALAY_HTTP_WRITER_H
#define GALAY_HTTP_WRITER_H

#include "HttpWriterSetting.h"
#include "HttpLog.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpError.h"
#include "galay-http/protoc/http/HttpChunk.h"
#include "galay-kernel/kernel/Awaitable.h"
#include "galay-kernel/kernel/Timeout.hpp"
#include "galay-kernel/async/TcpSocket.h"
#include <expected>
#include <coroutine>
#include <string>

namespace galay::http
{

using namespace galay::kernel;
using namespace galay::async;

// 前向声明
template<typename SocketType>
class HttpWriterImpl;

/**
 * @brief HTTP响应写入等待体
 * @tparam SocketType Socket类型（TcpSocket 或 SslSocket）
 */
template<typename SocketType>
class SendResponseAwaitableImpl : public galay::kernel::TimeoutSupport<SendResponseAwaitableImpl<SocketType>>
{
public:
    using SendAwaitableType = decltype(std::declval<SocketType>().send(std::declval<const char*>(), std::declval<size_t>()));

    SendResponseAwaitableImpl(HttpWriterImpl<SocketType>& writer, SendAwaitableType&& send_awaitable)
        : m_writer(writer)
        , m_send_awaitable(std::move(send_awaitable))
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    auto await_suspend(std::coroutine_handle<> handle) {
        return m_send_awaitable.await_suspend(handle);
    }

    std::expected<bool, HttpError> await_resume() {
        auto send_result = m_send_awaitable.await_resume();
        if (!send_result) {
            HTTP_LOG_DEBUG("send failed: {}", send_result.error().message());
            return std::unexpected(HttpError(kSendError, send_result.error().message()));
        }

        size_t bytes_written = send_result.value();
        m_writer.updateRemaining(bytes_written);

        if (m_writer.getRemainingBytes() == 0) {
            return true;
        }

        return false;
    }

private:
    HttpWriterImpl<SocketType>& m_writer;
    SendAwaitableType m_send_awaitable;

public:
    std::expected<bool, galay::kernel::IOError> m_result;
};

/**
 * @brief HTTP写入器模板类
 * @tparam SocketType Socket类型（TcpSocket 或 SslSocket）
 */
template<typename SocketType>
class HttpWriterImpl
{
public:
    HttpWriterImpl(const HttpWriterSetting& setting, SocketType& socket)
        : m_setting(setting)
        , m_socket(socket)
        , m_remaining_bytes(0)
    {
    }

    SendResponseAwaitableImpl<SocketType> sendResponse(HttpResponse& response) {
        if (m_remaining_bytes == 0) {
            m_buffer = response.toString();
            m_remaining_bytes = m_buffer.size();
        }

        size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
        const char* send_ptr = m_buffer.data() + sent_bytes;

        return SendResponseAwaitableImpl<SocketType>(*this, m_socket.send(send_ptr, m_remaining_bytes));
    }

    SendResponseAwaitableImpl<SocketType> sendRequest(HttpRequest& request) {
        if (m_remaining_bytes == 0) {
            m_buffer = request.toString();
            m_remaining_bytes = m_buffer.size();
        }

        size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
        const char* send_ptr = m_buffer.data() + sent_bytes;

        return SendResponseAwaitableImpl<SocketType>(*this, m_socket.send(send_ptr, m_remaining_bytes));
    }

    SendResponseAwaitableImpl<SocketType> sendHeader(HttpResponseHeader&& header) {
        if (m_remaining_bytes == 0) {
            m_buffer = header.toString();
            m_remaining_bytes = m_buffer.size();
        }

        size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
        const char* send_ptr = m_buffer.data() + sent_bytes;

        return SendResponseAwaitableImpl<SocketType>(*this, m_socket.send(send_ptr, m_remaining_bytes));
    }

    SendResponseAwaitableImpl<SocketType> sendHeader(HttpRequestHeader&& header) {
        if (m_remaining_bytes == 0) {
            m_buffer = header.toString();
            m_remaining_bytes = m_buffer.size();
        }

        size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
        const char* send_ptr = m_buffer.data() + sent_bytes;

        return SendResponseAwaitableImpl<SocketType>(*this, m_socket.send(send_ptr, m_remaining_bytes));
    }

    SendResponseAwaitableImpl<SocketType> send(std::string&& data) {
        if (m_remaining_bytes == 0) {
            m_buffer = std::move(data);
            m_remaining_bytes = m_buffer.size();
        }

        size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
        const char* send_ptr = m_buffer.data() + sent_bytes;

        return SendResponseAwaitableImpl<SocketType>(*this, m_socket.send(send_ptr, m_remaining_bytes));
    }

    SendResponseAwaitableImpl<SocketType> send(const char* buffer, size_t length) {
        return SendResponseAwaitableImpl<SocketType>(*this, m_socket.send(buffer, length));
    }

    SendResponseAwaitableImpl<SocketType> sendChunk(const std::string& data, bool is_last = false) {
        if (m_remaining_bytes == 0) {
            m_buffer = Chunk::toChunk(data, is_last);
            m_remaining_bytes = m_buffer.size();
        }

        size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
        const char* send_ptr = m_buffer.data() + sent_bytes;

        return SendResponseAwaitableImpl<SocketType>(*this, m_socket.send(send_ptr, m_remaining_bytes));
    }

    void updateRemaining(size_t bytes_sent) {
        if (bytes_sent >= m_remaining_bytes) {
            m_remaining_bytes = 0;
            m_buffer.clear();
        } else {
            m_remaining_bytes -= bytes_sent;
        }
    }

    size_t getRemainingBytes() const {
        return m_remaining_bytes;
    }

private:
    const HttpWriterSetting& m_setting;
    SocketType& m_socket;
    std::string m_buffer;
    size_t m_remaining_bytes;
};

// 类型别名 - HTTP (TcpSocket)
using SendResponseAwaitable = SendResponseAwaitableImpl<TcpSocket>;
using HttpWriter = HttpWriterImpl<TcpSocket>;

#ifdef GALAY_HTTP_SSL_ENABLED
// 类型别名 - HTTPS (SslSocket)
#include "galay-socket/async/SslSocket.h"
using SendResponseAwaitableSsl = SendResponseAwaitableImpl<galay::async::SslSocket>;
using HttpsWriter = HttpWriterImpl<galay::async::SslSocket>;
#endif

} // namespace galay::http

#endif // GALAY_HTTP_WRITER_H
