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
#include <sys/uio.h>

namespace galay::http
{

using namespace galay::kernel;
using namespace galay::async;

// 前向声明
template<typename SocketType>
class HttpWriterImpl;

// 类型萃取：判断是否为 TcpSocket
template<typename T>
struct is_tcp_socket : std::false_type {};

template<>
struct is_tcp_socket<TcpSocket> : std::true_type {};

template<typename T>
inline constexpr bool is_tcp_socket_v = is_tcp_socket<T>::value;

/**
 * @brief HTTP响应写入等待体（writev 优化版 - 仅用于 TcpSocket）
 */
template<typename SocketType>
class SendResponseWritevAwaitableImpl : public galay::kernel::TimeoutSupport<SendResponseWritevAwaitableImpl<SocketType>>
{
public:
    using WritevAwaitableType = decltype(std::declval<SocketType>().writev(std::declval<std::vector<iovec>>()));

    SendResponseWritevAwaitableImpl(HttpWriterImpl<SocketType>& writer, WritevAwaitableType&& writev_awaitable)
        : m_writer(&writer)
        , m_writev_awaitable(std::move(writev_awaitable))
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    auto await_suspend(std::coroutine_handle<> handle) {
        return m_writev_awaitable.await_suspend(handle);
    }

    std::expected<bool, HttpError> await_resume() {
        auto writev_result = m_writev_awaitable.await_resume();
        if (!writev_result) {
            HTTP_LOG_DEBUG("[writev] [fail] [{}]", writev_result.error().message());
            return std::unexpected(HttpError(kSendError, writev_result.error().message()));
        }

        size_t bytes_written = writev_result.value();
        m_writer->updateRemainingWritev(bytes_written);

        if (m_writer->getRemainingBytes() == 0) {
            return true;
        }

        return false;
    }

private:
    HttpWriterImpl<SocketType>* m_writer;
    WritevAwaitableType m_writev_awaitable;

public:
    std::expected<bool, galay::kernel::IOError> m_result;
};

/**
 * @brief HTTP响应写入等待体（send 版本 - 用于 SslSocket 等）
 */
template<typename SocketType>
class SendResponseAwaitableImpl : public galay::kernel::TimeoutSupport<SendResponseAwaitableImpl<SocketType>>
{
public:
    using SendAwaitableType = decltype(std::declval<SocketType>().send(std::declval<const char*>(), std::declval<size_t>()));

    SendResponseAwaitableImpl(HttpWriterImpl<SocketType>& writer, SendAwaitableType&& send_awaitable)
        : m_writer(&writer)
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
            HTTP_LOG_DEBUG("[send] [fail] [{}]", send_result.error().message());
            return std::unexpected(HttpError(kSendError, send_result.error().message()));
        }

        size_t bytes_written = send_result.value();
        m_writer->updateRemaining(bytes_written);

        if (m_writer->getRemainingBytes() == 0) {
            return true;
        }

        return false;
    }

private:
    HttpWriterImpl<SocketType>* m_writer;
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
        : m_setting(&setting)
        , m_socket(&socket)
        , m_remaining_bytes(0)
        , m_writev_offset(0)
    {
    }

    auto sendResponse(HttpResponse& response) {
        if (m_remaining_bytes == 0) {
            logResponseStatus(response.header().code());

            if constexpr (is_tcp_socket_v<SocketType>) {
                // TcpSocket: 使用 writev 避免内存拷贝
                m_body_buffer = response.getBodyStr();

                if(!response.header().isChunked()) {
                    response.header().headerPairs().addHeaderPairIfNotExist("Content-Length", std::to_string(m_body_buffer.size()));
                }

                m_buffer = response.header().toString();

                m_iovecs.clear();
                m_iovecs.push_back({const_cast<char*>(m_buffer.data()), m_buffer.size()});
                if (!m_body_buffer.empty()) {
                    m_iovecs.push_back({const_cast<char*>(m_body_buffer.data()), m_body_buffer.size()});
                }

                m_remaining_bytes = m_buffer.size() + m_body_buffer.size();
                m_writev_offset = 0;
            } else {
                // SslSocket: 使用 send
                m_buffer = response.toString();
                m_remaining_bytes = m_buffer.size();
            }
        }

        if constexpr (is_tcp_socket_v<SocketType>) {
            return SendResponseWritevAwaitableImpl<SocketType>(*this, m_socket->writev(m_iovecs));
        } else {
            size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
            const char* send_ptr = m_buffer.data() + sent_bytes;
            return SendResponseAwaitableImpl<SocketType>(*this, m_socket->send(send_ptr, m_remaining_bytes));
        }
    }

    SendResponseAwaitableImpl<SocketType> sendRequest(HttpRequest& request) {
        if (m_remaining_bytes == 0) {
            m_buffer = request.toString();
            m_remaining_bytes = m_buffer.size();
        }

        size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
        const char* send_ptr = m_buffer.data() + sent_bytes;

        return SendResponseAwaitableImpl<SocketType>(*this, m_socket->send(send_ptr, m_remaining_bytes));
    }

    SendResponseAwaitableImpl<SocketType> sendHeader(HttpResponseHeader&& header) {
        if (m_remaining_bytes == 0) {
            logResponseStatus(header.code());
            m_buffer = header.toString();
            m_remaining_bytes = m_buffer.size();
        }

        size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
        const char* send_ptr = m_buffer.data() + sent_bytes;

        return SendResponseAwaitableImpl<SocketType>(*this, m_socket->send(send_ptr, m_remaining_bytes));
    }

    SendResponseAwaitableImpl<SocketType> sendHeader(HttpRequestHeader&& header) {
        if (m_remaining_bytes == 0) {
            m_buffer = header.toString();
            m_remaining_bytes = m_buffer.size();
        }

        size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
        const char* send_ptr = m_buffer.data() + sent_bytes;

        return SendResponseAwaitableImpl<SocketType>(*this, m_socket->send(send_ptr, m_remaining_bytes));
    }

    SendResponseAwaitableImpl<SocketType> send(std::string&& data) {
        if (m_remaining_bytes == 0) {
            m_buffer = std::move(data);
            m_remaining_bytes = m_buffer.size();
        }

        size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
        const char* send_ptr = m_buffer.data() + sent_bytes;

        return SendResponseAwaitableImpl<SocketType>(*this, m_socket->send(send_ptr, m_remaining_bytes));
    }

    SendResponseAwaitableImpl<SocketType> send(const char* buffer, size_t length) {
        return SendResponseAwaitableImpl<SocketType>(*this, m_socket->send(buffer, length));
    }

    SendResponseAwaitableImpl<SocketType> sendChunk(const std::string& data, bool is_last = false) {
        if (m_remaining_bytes == 0) {
            m_buffer = Chunk::toChunk(data, is_last);
            m_remaining_bytes = m_buffer.size();
        }

        size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
        const char* send_ptr = m_buffer.data() + sent_bytes;

        return SendResponseAwaitableImpl<SocketType>(*this, m_socket->send(send_ptr, m_remaining_bytes));
    }

    void updateRemaining(size_t bytes_sent) {
        if (bytes_sent >= m_remaining_bytes) {
            m_remaining_bytes = 0;
            m_buffer.clear();
        } else {
            m_remaining_bytes -= bytes_sent;
        }
    }

    void updateRemainingWritev(size_t bytes_sent) {
        if (bytes_sent >= m_remaining_bytes) {
            m_remaining_bytes = 0;
            m_buffer.clear();
            m_body_buffer.clear();
            m_iovecs.clear();
            m_writev_offset = 0;
        } else {
            m_remaining_bytes -= bytes_sent;
            m_writev_offset += bytes_sent;

            // 更新 iovec 数组，跳过已发送的部分
            size_t offset = m_writev_offset;
            m_iovecs.clear();

            size_t header_size = m_buffer.size();
            if (offset < header_size) {
                // 还在 header 部分
                m_iovecs.push_back({const_cast<char*>(m_buffer.data() + offset), header_size - offset});
                if (!m_body_buffer.empty()) {
                    m_iovecs.push_back({const_cast<char*>(m_body_buffer.data()), m_body_buffer.size()});
                }
            } else {
                // 已经到 body 部分
                size_t body_offset = offset - header_size;
                if (body_offset < m_body_buffer.size()) {
                    m_iovecs.push_back({const_cast<char*>(m_body_buffer.data() + body_offset), m_body_buffer.size() - body_offset});
                }
            }
        }
    }

    size_t getRemainingBytes() const {
        return m_remaining_bytes;
    }

private:
    static void logResponseStatus(HttpStatusCode code) {
        const int status = static_cast<int>(code);
        if (status >= 500) {
            HTTP_LOG_ERROR("[{}] [{}]", status, httpStatusCodeToString(code));
        } else if (status >= 400) {
            HTTP_LOG_WARN("[{}] [{}]", status, httpStatusCodeToString(code));
        } else {
            HTTP_LOG_INFO("[{}] [{}]", status, httpStatusCodeToString(code));
        }
    }

    const HttpWriterSetting* m_setting;
    SocketType* m_socket;
    std::string m_buffer;        // TcpSocket: 存储 header; SslSocket: 存储完整响应
    size_t m_remaining_bytes;

    // writev 专用成员（仅 TcpSocket 使用）
    std::string m_body_buffer;   // 存储 body
    std::vector<iovec> m_iovecs;
    size_t m_writev_offset;
};

// 类型别名 - HTTP (TcpSocket)
using SendResponseAwaitable = SendResponseAwaitableImpl<TcpSocket>;
using HttpWriter = HttpWriterImpl<TcpSocket>;

} // namespace galay::http

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/SslSocket.h"
namespace galay::http {
using SendResponseAwaitableSsl = SendResponseAwaitableImpl<galay::ssl::SslSocket>;
using HttpsWriter = HttpWriterImpl<galay::ssl::SslSocket>;
} // namespace galay::http
#endif

#endif // GALAY_HTTP_WRITER_H
