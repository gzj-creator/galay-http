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
#include <optional>
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

template<typename SocketType, bool IsTcp = is_tcp_socket_v<SocketType>>
class SendResponseAwaitableImpl;

/**
 * @brief HTTP响应写入等待体（writev 优化版 - 仅用于 TcpSocket）
 */
template<typename SocketType>
class SendResponseWritevAwaitableImpl
    : public galay::kernel::CustomAwaitable
    , public galay::kernel::TimeoutSupport<SendResponseWritevAwaitableImpl<SocketType>>
{
public:
    class ProtocolWritevAwaitable : public galay::kernel::WritevAwaitable
    {
    public:
        explicit ProtocolWritevAwaitable(SendResponseWritevAwaitableImpl* owner)
            : galay::kernel::WritevAwaitable(owner->m_socket->controller(), owner->m_writer->getIovecsCopy())
            , m_owner(owner)
        {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle) override {
            if (m_owner->m_writer->getRemainingBytes() == 0) {
                return true;
            }

            if (cqe == nullptr) {
                syncIovecs();
                if (m_iovecs.empty()) {
                    m_owner->setSendError(HttpError(kSendError, "No data to write"));
                    return true;
                }
                return false;
            }

            auto result = galay::kernel::io::handleWritev(cqe);
            if (!result && galay::kernel::IOError::contains(result.error().code(), galay::kernel::kNotReady)) {
                return false;
            }
            if (!result) {
                m_owner->setSendError(result.error());
                return true;
            }

            size_t written = result.value();
            if (written == 0) {
                return false;
            }

            m_owner->m_writer->updateRemainingWritev(written);
            if (m_owner->m_writer->getRemainingBytes() == 0) {
                return true;
            }

            syncIovecs();
            if (m_iovecs.empty()) {
                m_owner->setSendError(HttpError(kSendError, "No remaining iovec to write"));
                return true;
            }
            return false;
        }
#else
        bool handleComplete(GHandle handle) override {
            while (m_owner->m_writer->getRemainingBytes() > 0) {
                syncIovecs();
                if (m_iovecs.empty()) {
                    m_owner->setSendError(HttpError(kSendError, "No remaining iovec to write"));
                    return true;
                }

                auto result = galay::kernel::io::handleWritev(handle, m_iovecs.data(), static_cast<int>(m_iovecs.size()));
                if (!result && galay::kernel::IOError::contains(result.error().code(), galay::kernel::kNotReady)) {
                    return false;
                }
                if (!result) {
                    m_owner->setSendError(result.error());
                    return true;
                }

                size_t written = result.value();
                if (written == 0) {
                    return false;
                }

                m_owner->m_writer->updateRemainingWritev(written);
            }
            return true;
        }
#endif

    private:
        void syncIovecs() {
            m_iovecs = m_owner->m_writer->getIovecsCopy();
        }

        SendResponseWritevAwaitableImpl* m_owner;
    };

    SendResponseWritevAwaitableImpl(HttpWriterImpl<SocketType>& writer, SocketType& socket)
        : galay::kernel::CustomAwaitable(socket.controller())
        , m_writer(&writer)
        , m_socket(&socket)
        , m_writev_awaitable(this)
        , m_result(true)
    {
        addTask(IOEventType::WRITEV, &m_writev_awaitable);
    }

    bool await_ready() const noexcept {
        return false;
    }

    using galay::kernel::CustomAwaitable::await_suspend;

    std::expected<bool, HttpError> await_resume() {
        onCompleted();

        if (!m_result.has_value()) {
            HTTP_LOG_DEBUG("[writev] [fail] [{}]", m_result.error().message());
            return std::unexpected(HttpError(kSendError, m_result.error().message()));
        }

        if (m_http_error.has_value()) {
            return std::unexpected(std::move(*m_http_error));
        }

        return true;
    }

private:
    HttpWriterImpl<SocketType>* m_writer;
    SocketType* m_socket;
    ProtocolWritevAwaitable m_writev_awaitable;
    std::optional<HttpError> m_http_error;

    void setSendError(const galay::kernel::IOError& io_error) {
        m_http_error = HttpError(kSendError, io_error.message());
    }

    void setSendError(HttpError&& error) {
        m_http_error = std::move(error);
    }

public:
    std::expected<bool, galay::kernel::IOError> m_result;
};

/**
 * @brief HTTP响应写入等待体（send 版本 - 用于 SslSocket 等）
 */
template<typename SocketType>
class SendResponseAwaitableImpl<SocketType, false> : public galay::kernel::TimeoutSupport<SendResponseAwaitableImpl<SocketType, false>>
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
 * @brief HTTP响应写入等待体（send 版本 - TcpSocket，链式发送完成唤醒）
 */
template<typename SocketType>
class SendResponseAwaitableImpl<SocketType, true>
    : public galay::kernel::CustomAwaitable
    , public galay::kernel::TimeoutSupport<SendResponseAwaitableImpl<SocketType, true>>
{
public:
    class ProtocolSendAwaitable : public galay::kernel::SendAwaitable
    {
    public:
        explicit ProtocolSendAwaitable(SendResponseAwaitableImpl* owner)
            : galay::kernel::SendAwaitable(owner->m_socket->controller(),
                                           owner->m_writer->bufferData() + owner->m_writer->sentBytes(),
                                           owner->m_writer->getRemainingBytes())
            , m_owner(owner)
        {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle) override {
            if (m_owner->m_writer->getRemainingBytes() == 0) {
                return true;
            }

            if (cqe == nullptr) {
                syncSendWindow();
                return m_length == 0;
            }

            auto result = galay::kernel::io::handleSend(cqe);
            if (!result && galay::kernel::IOError::contains(result.error().code(), galay::kernel::kNotReady)) {
                return false;
            }
            if (!result) {
                m_owner->setSendError(result.error());
                return true;
            }

            size_t written = result.value();
            if (written == 0) {
                return false;
            }

            m_owner->m_writer->updateRemaining(written);
            if (m_owner->m_writer->getRemainingBytes() == 0) {
                return true;
            }

            syncSendWindow();
            return false;
        }
#else
        bool handleComplete(GHandle handle) override {
            while (m_owner->m_writer->getRemainingBytes() > 0) {
                syncSendWindow();
                auto result = galay::kernel::io::handleSend(handle, m_buffer, m_length);
                if (!result && galay::kernel::IOError::contains(result.error().code(), galay::kernel::kNotReady)) {
                    return false;
                }
                if (!result) {
                    m_owner->setSendError(result.error());
                    return true;
                }

                size_t written = result.value();
                if (written == 0) {
                    return false;
                }

                m_owner->m_writer->updateRemaining(written);
            }
            return true;
        }
#endif

    private:
        void syncSendWindow() {
            if (m_owner->m_writer->getRemainingBytes() == 0) {
                m_buffer = nullptr;
                m_length = 0;
                return;
            }
            m_buffer = m_owner->m_writer->bufferData() + m_owner->m_writer->sentBytes();
            m_length = m_owner->m_writer->getRemainingBytes();
        }

        SendResponseAwaitableImpl* m_owner;
    };

    SendResponseAwaitableImpl(HttpWriterImpl<SocketType>& writer, SocketType& socket)
        : galay::kernel::CustomAwaitable(socket.controller())
        , m_writer(&writer)
        , m_socket(&socket)
        , m_send_awaitable(this)
        , m_has_send_task(false)
        , m_result(true)
    {
        if (m_writer->getRemainingBytes() > 0) {
            addTask(IOEventType::SEND, &m_send_awaitable);
            m_has_send_task = true;
        }
    }

    bool await_ready() const noexcept {
        return m_writer->getRemainingBytes() == 0;
    }

    using galay::kernel::CustomAwaitable::await_suspend;

    std::expected<bool, HttpError> await_resume() {
        if (!m_has_send_task) {
            return true;
        }

        onCompleted();

        if (!m_result.has_value()) {
            HTTP_LOG_DEBUG("[send] [fail] [{}]", m_result.error().message());
            return std::unexpected(HttpError(kSendError, m_result.error().message()));
        }
        if (m_http_error.has_value()) {
            return std::unexpected(std::move(*m_http_error));
        }

        return true;
    }

private:
    void setSendError(const galay::kernel::IOError& io_error) {
        m_http_error = HttpError(kSendError, io_error.message());
    }

    HttpWriterImpl<SocketType>* m_writer;
    SocketType* m_socket;
    ProtocolSendAwaitable m_send_awaitable;
    bool m_has_send_task;
    std::optional<HttpError> m_http_error;

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
            return SendResponseWritevAwaitableImpl<SocketType>(*this, *m_socket);
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

        if constexpr (is_tcp_socket_v<SocketType>) {
            return SendResponseAwaitableImpl<SocketType>(*this, *m_socket);
        } else {
            size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
            const char* send_ptr = m_buffer.data() + sent_bytes;
            return SendResponseAwaitableImpl<SocketType>(*this, m_socket->send(send_ptr, m_remaining_bytes));
        }
    }

    SendResponseAwaitableImpl<SocketType> sendHeader(HttpResponseHeader&& header) {
        if (m_remaining_bytes == 0) {
            logResponseStatus(header.code());
            m_buffer = header.toString();
            m_remaining_bytes = m_buffer.size();
        }

        if constexpr (is_tcp_socket_v<SocketType>) {
            return SendResponseAwaitableImpl<SocketType>(*this, *m_socket);
        } else {
            size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
            const char* send_ptr = m_buffer.data() + sent_bytes;
            return SendResponseAwaitableImpl<SocketType>(*this, m_socket->send(send_ptr, m_remaining_bytes));
        }
    }

    SendResponseAwaitableImpl<SocketType> sendHeader(HttpRequestHeader&& header) {
        if (m_remaining_bytes == 0) {
            m_buffer = header.toString();
            m_remaining_bytes = m_buffer.size();
        }

        if constexpr (is_tcp_socket_v<SocketType>) {
            return SendResponseAwaitableImpl<SocketType>(*this, *m_socket);
        } else {
            size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
            const char* send_ptr = m_buffer.data() + sent_bytes;
            return SendResponseAwaitableImpl<SocketType>(*this, m_socket->send(send_ptr, m_remaining_bytes));
        }
    }

    SendResponseAwaitableImpl<SocketType> send(std::string&& data) {
        if (m_remaining_bytes == 0) {
            m_buffer = std::move(data);
            m_remaining_bytes = m_buffer.size();
        }

        if constexpr (is_tcp_socket_v<SocketType>) {
            return SendResponseAwaitableImpl<SocketType>(*this, *m_socket);
        } else {
            size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
            const char* send_ptr = m_buffer.data() + sent_bytes;
            return SendResponseAwaitableImpl<SocketType>(*this, m_socket->send(send_ptr, m_remaining_bytes));
        }
    }

    SendResponseAwaitableImpl<SocketType> send(const char* buffer, size_t length) {
        if (m_remaining_bytes == 0) {
            m_buffer.assign(buffer, length);
            m_remaining_bytes = m_buffer.size();
        }

        if constexpr (is_tcp_socket_v<SocketType>) {
            return SendResponseAwaitableImpl<SocketType>(*this, *m_socket);
        } else {
            size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
            const char* send_ptr = m_buffer.data() + sent_bytes;
            return SendResponseAwaitableImpl<SocketType>(*this, m_socket->send(send_ptr, m_remaining_bytes));
        }
    }

    SendResponseAwaitableImpl<SocketType> sendChunk(const std::string& data, bool is_last = false) {
        if (m_remaining_bytes == 0) {
            m_buffer = Chunk::toChunk(data, is_last);
            m_remaining_bytes = m_buffer.size();
        }

        if constexpr (is_tcp_socket_v<SocketType>) {
            return SendResponseAwaitableImpl<SocketType>(*this, *m_socket);
        } else {
            size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
            const char* send_ptr = m_buffer.data() + sent_bytes;
            return SendResponseAwaitableImpl<SocketType>(*this, m_socket->send(send_ptr, m_remaining_bytes));
        }
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

    const char* bufferData() const {
        return m_buffer.data();
    }

    size_t sentBytes() const {
        return m_buffer.size() - m_remaining_bytes;
    }

    std::vector<iovec> getIovecsCopy() const {
        return m_iovecs;
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

    HttpWriterSetting m_setting;
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
#include "galay-ssl/async/SslSocket.h"
namespace galay::http {
using SendResponseAwaitableSsl = SendResponseAwaitableImpl<galay::ssl::SslSocket>;
using HttpsWriter = HttpWriterImpl<galay::ssl::SslSocket>;
} // namespace galay::http
#endif

#endif // GALAY_HTTP_WRITER_H
