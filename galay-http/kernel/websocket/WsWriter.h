#ifndef GALAY_WS_WRITER_H
#define GALAY_WS_WRITER_H

#include "WsWriterSetting.h"
#include "galay-http/protoc/websocket/WebSocketFrame.h"
#include "galay-http/protoc/websocket/WebSocketError.h"
#include "galay-kernel/kernel/Awaitable.h"
#include "galay-kernel/kernel/Timeout.hpp"
#include "galay-kernel/async/TcpSocket.h"
#include <expected>
#include <coroutine>
#include <optional>
#include <string>
#include <sys/uio.h>

namespace galay::websocket
{

using namespace galay::kernel;
using namespace galay::async;

// 前向声明
template<typename SocketType>
class WsWriterImpl;

// 类型萃取：判断是否为 TcpSocket
template<typename T>
struct is_tcp_socket : std::false_type {};

template<>
struct is_tcp_socket<TcpSocket> : std::true_type {};

template<typename T>
inline constexpr bool is_tcp_socket_v = is_tcp_socket<T>::value;

template<typename SocketType, bool IsTcp = is_tcp_socket_v<SocketType>>
class SendFrameAwaitableImpl;

/**
 * @brief WebSocket帧发送等待体（TcpSocket: CustomAwaitable + WritevAwaitable）
 */
template<typename SocketType>
class SendFrameAwaitableImpl<SocketType, true>
    : public galay::kernel::CustomAwaitable
    , public galay::kernel::TimeoutSupport<SendFrameAwaitableImpl<SocketType, true>>
{
public:
    class ProtocolWritevAwaitable : public galay::kernel::WritevAwaitable
    {
    public:
        explicit ProtocolWritevAwaitable(SendFrameAwaitableImpl* owner)
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
                    m_owner->setSendError(WsError(kWsSendError, "No data to write"));
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

            size_t bytes_written = result.value();
            if (bytes_written == 0) {
                return false;
            }

            m_owner->m_writer->updateRemainingWritev(bytes_written);
            if (m_owner->m_writer->getRemainingBytes() == 0) {
                return true;
            }

            syncIovecs();
            if (m_iovecs.empty()) {
                m_owner->setSendError(WsError(kWsSendError, "No remaining iovec to write"));
                return true;
            }
            return false;
        }
#else
        bool handleComplete(GHandle handle) override {
            while (m_owner->m_writer->getRemainingBytes() > 0) {
                syncIovecs();
                if (m_iovecs.empty()) {
                    m_owner->setSendError(WsError(kWsSendError, "No remaining iovec to write"));
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

                size_t bytes_written = result.value();
                if (bytes_written == 0) {
                    return false;
                }

                m_owner->m_writer->updateRemainingWritev(bytes_written);
            }

            return true;
        }
#endif

    private:
        void syncIovecs() {
            m_iovecs = m_owner->m_writer->getIovecsCopy();
        }

        SendFrameAwaitableImpl* m_owner;
    };

    SendFrameAwaitableImpl(WsWriterImpl<SocketType>& writer, SocketType& socket)
        : galay::kernel::CustomAwaitable(socket.controller())
        , m_writer(&writer)
        , m_socket(&socket)
        , m_writev_awaitable(this)
        , m_result(0)
    {
        addTask(IOEventType::WRITEV, &m_writev_awaitable);
    }

    bool await_ready() const noexcept {
        return false;
    }

    using galay::kernel::CustomAwaitable::await_suspend;

    std::expected<bool, WsError> await_resume() {
        onCompleted();

        if (!m_result.has_value()) {
            return std::unexpected(WsError(kWsSendError, m_result.error().message()));
        }

        if (m_ws_error.has_value()) {
            return std::unexpected(std::move(*m_ws_error));
        }

        return true;
    }

private:
    void setSendError(const galay::kernel::IOError& io_error) {
        m_ws_error = WsError(kWsSendError, io_error.message());
    }

    void setSendError(WsError&& ws_error) {
        m_ws_error = std::move(ws_error);
    }

    WsWriterImpl<SocketType>* m_writer;
    SocketType* m_socket;
    ProtocolWritevAwaitable m_writev_awaitable;
    std::optional<WsError> m_ws_error;

public:
    std::expected<size_t, galay::kernel::IOError> m_result;
};

/**
 * @brief WebSocket帧发送等待体（非 TcpSocket: send 版本）
 */
template<typename SocketType>
class SendFrameAwaitableImpl<SocketType, false>
    : public galay::kernel::TimeoutSupport<SendFrameAwaitableImpl<SocketType, false>>
{
public:
    using SendAwaitableType = decltype(std::declval<SocketType>().send(std::declval<const char*>(), std::declval<size_t>()));

    SendFrameAwaitableImpl(WsWriterImpl<SocketType>& writer, SendAwaitableType&& send_awaitable)
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

    std::expected<bool, WsError> await_resume() {
        auto send_result = m_send_awaitable.await_resume();
        if (!send_result) {
            return std::unexpected(WsError(kWsSendError, send_result.error().message()));
        }

        size_t bytes_written = send_result.value();
        m_writer.updateRemaining(bytes_written);

        if (m_writer.getRemainingBytes() == 0) {
            return true;
        }
        return false;
    }

private:
    WsWriterImpl<SocketType>& m_writer;
    SendAwaitableType m_send_awaitable;

public:
    std::expected<size_t, galay::kernel::IOError> m_result;
};

/**
 * @brief WebSocket写入器模板类
 */
template<typename SocketType>
class WsWriterImpl
{
public:
    WsWriterImpl(const WsWriterSetting& setting, SocketType& socket)
        : m_setting(setting)
        , m_socket(&socket)
        , m_remaining_bytes(0)
        , m_writev_offset(0)
    {
    }

    auto sendText(const std::string& text, bool fin = true) {
        if (m_remaining_bytes == 0) {
            WsFrame frame = WsFrameParser::createTextFrame(text, fin);

            if constexpr (is_tcp_socket_v<SocketType>) {
                // TcpSocket: 使用 writev 避免内存拷贝
                m_buffer = WsFrameParser::toBytesHeader(frame, m_setting.use_mask, m_masking_key);
                m_payload_buffer = frame.payload;

                // 如果需要掩码，应用掩码
                if (m_setting.use_mask) {
                    WsFrameParser::applyMask(m_payload_buffer, m_masking_key);
                }

                // 准备 iovec 数组
                m_iovecs.clear();
                m_iovecs.push_back({const_cast<char*>(m_buffer.data()), m_buffer.size()});
                if (!m_payload_buffer.empty()) {
                    m_iovecs.push_back({const_cast<char*>(m_payload_buffer.data()), m_payload_buffer.size()});
                }

                m_remaining_bytes = m_buffer.size() + m_payload_buffer.size();
                m_writev_offset = 0;
            } else {
                // SslSocket: 使用 send
                m_buffer = WsFrameParser::toBytes(frame, m_setting.use_mask);
                m_remaining_bytes = m_buffer.size();
            }
        }

        if constexpr (is_tcp_socket_v<SocketType>) {
            return SendFrameAwaitableImpl<SocketType>(*this, *m_socket);
        } else {
            size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
            const char* send_ptr = m_buffer.data() + sent_bytes;
            return SendFrameAwaitableImpl<SocketType>(*this, m_socket->send(send_ptr, m_remaining_bytes));
        }
    }

    auto sendBinary(const std::string& data, bool fin = true) {
        if (m_remaining_bytes == 0) {
            WsFrame frame = WsFrameParser::createBinaryFrame(data, fin);

            if constexpr (is_tcp_socket_v<SocketType>) {
                // TcpSocket: 使用 writev 避免内存拷贝
                m_buffer = WsFrameParser::toBytesHeader(frame, m_setting.use_mask, m_masking_key);
                m_payload_buffer = frame.payload;

                // 如果需要掩码，应用掩码
                if (m_setting.use_mask) {
                    WsFrameParser::applyMask(m_payload_buffer, m_masking_key);
                }

                // 准备 iovec 数组
                m_iovecs.clear();
                m_iovecs.push_back({const_cast<char*>(m_buffer.data()), m_buffer.size()});
                if (!m_payload_buffer.empty()) {
                    m_iovecs.push_back({const_cast<char*>(m_payload_buffer.data()), m_payload_buffer.size()});
                }

                m_remaining_bytes = m_buffer.size() + m_payload_buffer.size();
                m_writev_offset = 0;
            } else {
                // SslSocket: 使用 send
                m_buffer = WsFrameParser::toBytes(frame, m_setting.use_mask);
                m_remaining_bytes = m_buffer.size();
            }
        }

        if constexpr (is_tcp_socket_v<SocketType>) {
            return SendFrameAwaitableImpl<SocketType>(*this, *m_socket);
        } else {
            size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
            const char* send_ptr = m_buffer.data() + sent_bytes;
            return SendFrameAwaitableImpl<SocketType>(*this, m_socket->send(send_ptr, m_remaining_bytes));
        }
    }

    auto sendPing(const std::string& data = "") {
        if (m_remaining_bytes == 0) {
            WsFrame frame = WsFrameParser::createPingFrame(data);

            if constexpr (is_tcp_socket_v<SocketType>) {
                // TcpSocket: 使用 writev 避免内存拷贝
                m_buffer = WsFrameParser::toBytesHeader(frame, m_setting.use_mask, m_masking_key);
                m_payload_buffer = frame.payload;

                // 如果需要掩码，应用掩码
                if (m_setting.use_mask) {
                    WsFrameParser::applyMask(m_payload_buffer, m_masking_key);
                }

                // 准备 iovec 数组
                m_iovecs.clear();
                m_iovecs.push_back({const_cast<char*>(m_buffer.data()), m_buffer.size()});
                if (!m_payload_buffer.empty()) {
                    m_iovecs.push_back({const_cast<char*>(m_payload_buffer.data()), m_payload_buffer.size()});
                }

                m_remaining_bytes = m_buffer.size() + m_payload_buffer.size();
                m_writev_offset = 0;
            } else {
                // SslSocket: 使用 send
                m_buffer = WsFrameParser::toBytes(frame, m_setting.use_mask);
                m_remaining_bytes = m_buffer.size();
            }
        }

        if constexpr (is_tcp_socket_v<SocketType>) {
            return SendFrameAwaitableImpl<SocketType>(*this, *m_socket);
        } else {
            size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
            const char* send_ptr = m_buffer.data() + sent_bytes;
            return SendFrameAwaitableImpl<SocketType>(*this, m_socket->send(send_ptr, m_remaining_bytes));
        }
    }

    auto sendPong(const std::string& data = "") {
        if (m_remaining_bytes == 0) {
            WsFrame frame = WsFrameParser::createPongFrame(data);

            if constexpr (is_tcp_socket_v<SocketType>) {
                // TcpSocket: 使用 writev 避免内存拷贝
                m_buffer = WsFrameParser::toBytesHeader(frame, m_setting.use_mask, m_masking_key);
                m_payload_buffer = frame.payload;

                // 如果需要掩码，应用掩码
                if (m_setting.use_mask) {
                    WsFrameParser::applyMask(m_payload_buffer, m_masking_key);
                }

                // 准备 iovec 数组
                m_iovecs.clear();
                m_iovecs.push_back({const_cast<char*>(m_buffer.data()), m_buffer.size()});
                if (!m_payload_buffer.empty()) {
                    m_iovecs.push_back({const_cast<char*>(m_payload_buffer.data()), m_payload_buffer.size()});
                }

                m_remaining_bytes = m_buffer.size() + m_payload_buffer.size();
                m_writev_offset = 0;
            } else {
                // SslSocket: 使用 send
                m_buffer = WsFrameParser::toBytes(frame, m_setting.use_mask);
                m_remaining_bytes = m_buffer.size();
            }
        }

        if constexpr (is_tcp_socket_v<SocketType>) {
            return SendFrameAwaitableImpl<SocketType>(*this, *m_socket);
        } else {
            size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
            const char* send_ptr = m_buffer.data() + sent_bytes;
            return SendFrameAwaitableImpl<SocketType>(*this, m_socket->send(send_ptr, m_remaining_bytes));
        }
    }

    auto sendClose(WsCloseCode code = WsCloseCode::Normal, const std::string& reason = "") {
        if (m_remaining_bytes == 0) {
            WsFrame frame = WsFrameParser::createCloseFrame(code, reason);

            if constexpr (is_tcp_socket_v<SocketType>) {
                // TcpSocket: 使用 writev 避免内存拷贝
                m_buffer = WsFrameParser::toBytesHeader(frame, m_setting.use_mask, m_masking_key);
                m_payload_buffer = frame.payload;

                // 如果需要掩码，应用掩码
                if (m_setting.use_mask) {
                    WsFrameParser::applyMask(m_payload_buffer, m_masking_key);
                }

                // 准备 iovec 数组
                m_iovecs.clear();
                m_iovecs.push_back({const_cast<char*>(m_buffer.data()), m_buffer.size()});
                if (!m_payload_buffer.empty()) {
                    m_iovecs.push_back({const_cast<char*>(m_payload_buffer.data()), m_payload_buffer.size()});
                }

                m_remaining_bytes = m_buffer.size() + m_payload_buffer.size();
                m_writev_offset = 0;
            } else {
                // SslSocket: 使用 send
                m_buffer = WsFrameParser::toBytes(frame, m_setting.use_mask);
                m_remaining_bytes = m_buffer.size();
            }
        }

        if constexpr (is_tcp_socket_v<SocketType>) {
            return SendFrameAwaitableImpl<SocketType>(*this, *m_socket);
        } else {
            size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
            const char* send_ptr = m_buffer.data() + sent_bytes;
            return SendFrameAwaitableImpl<SocketType>(*this, m_socket->send(send_ptr, m_remaining_bytes));
        }
    }

    auto sendFrame(const WsFrame& frame) {
        if (m_remaining_bytes == 0) {
            if constexpr (is_tcp_socket_v<SocketType>) {
                // TcpSocket: 使用 writev 避免内存拷贝
                m_buffer = WsFrameParser::toBytesHeader(frame, m_setting.use_mask, m_masking_key);
                m_payload_buffer = frame.payload;

                // 如果需要掩码，应用掩码
                if (m_setting.use_mask) {
                    WsFrameParser::applyMask(m_payload_buffer, m_masking_key);
                }

                // 准备 iovec 数组
                m_iovecs.clear();
                m_iovecs.push_back({const_cast<char*>(m_buffer.data()), m_buffer.size()});
                if (!m_payload_buffer.empty()) {
                    m_iovecs.push_back({const_cast<char*>(m_payload_buffer.data()), m_payload_buffer.size()});
                }

                m_remaining_bytes = m_buffer.size() + m_payload_buffer.size();
                m_writev_offset = 0;
            } else {
                // SslSocket: 使用 send
                m_buffer = WsFrameParser::toBytes(frame, m_setting.use_mask);
                m_remaining_bytes = m_buffer.size();
            }
        }

        if constexpr (is_tcp_socket_v<SocketType>) {
            return SendFrameAwaitableImpl<SocketType>(*this, *m_socket);
        } else {
            size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
            const char* send_ptr = m_buffer.data() + sent_bytes;
            return SendFrameAwaitableImpl<SocketType>(*this, m_socket->send(send_ptr, m_remaining_bytes));
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
            m_payload_buffer.clear();
            m_iovecs.clear();
            m_writev_offset = 0;
            return;
        }

        m_remaining_bytes -= bytes_sent;

        if (m_remaining_bytes == 0) {
            m_buffer.clear();
            m_payload_buffer.clear();
            m_iovecs.clear();
            m_writev_offset = 0;
            return;
        }

        // 更新 iovec 数组，跳过已发送的部分
        size_t consumed = bytes_sent;
        m_iovecs.clear();

        // 检查 header 是否已完全发送
        if (consumed < m_buffer.size()) {
            // header 还有剩余
            m_iovecs.push_back({const_cast<char*>(m_buffer.data()) + consumed, m_buffer.size() - consumed});
            if (!m_payload_buffer.empty()) {
                m_iovecs.push_back({const_cast<char*>(m_payload_buffer.data()), m_payload_buffer.size()});
            }
        } else {
            // header 已完全发送，只剩 payload
            consumed -= m_buffer.size();
            if (consumed < m_payload_buffer.size()) {
                m_iovecs.push_back({const_cast<char*>(m_payload_buffer.data()) + consumed, m_payload_buffer.size() - consumed});
            }
        }
    }

    size_t getRemainingBytes() const {
        return m_remaining_bytes;
    }

    std::vector<iovec> getIovecsCopy() const {
        return m_iovecs;
    }

private:
    WsWriterSetting m_setting;
    SocketType* m_socket;
    std::string m_buffer;           // 存储 header
    std::string m_payload_buffer;   // 存储 payload（用于 writev）
    std::vector<iovec> m_iovecs;    // iovec 数组（用于 writev）
    size_t m_remaining_bytes;
    size_t m_writev_offset;         // writev 偏移量
    uint8_t m_masking_key[4];       // 掩码密钥
};

// 类型别名 - WebSocket over TCP
using SendFrameAwaitable = SendFrameAwaitableImpl<TcpSocket>;
using WsWriter = WsWriterImpl<TcpSocket>;

} // namespace galay::websocket

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/async/SslSocket.h"
namespace galay::websocket {
using SendFrameAwaitableSsl = SendFrameAwaitableImpl<galay::ssl::SslSocket>;
using WssWriter = WsWriterImpl<galay::ssl::SslSocket>;
} // namespace galay::websocket
#endif

#endif // GALAY_WS_WRITER_H
