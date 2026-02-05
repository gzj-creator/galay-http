#ifndef GALAY_WS_WRITER_H
#define GALAY_WS_WRITER_H

#include "WsWriterSetting.h"
#include "galay-http/protoc/websocket/WebSocketFrame.h"
#include "galay-http/protoc/websocket/WebSocketError.h"
#include "galay-kernel/kernel/Timeout.hpp"
#include "galay-kernel/async/TcpSocket.h"
#include <expected>
#include <coroutine>
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

/**
 * @brief WebSocket帧发送等待体（writev 优化版 - 仅用于 TcpSocket）
 */
template<typename SocketType>
class SendFrameWritevAwaitableImpl : public galay::kernel::TimeoutSupport<SendFrameWritevAwaitableImpl<SocketType>>
{
public:
    using WritevAwaitableType = decltype(std::declval<SocketType>().writev(std::declval<std::vector<iovec>>()));

    SendFrameWritevAwaitableImpl(WsWriterImpl<SocketType>& writer, WritevAwaitableType&& writev_awaitable)
        : m_writer(writer)
        , m_writev_awaitable(std::move(writev_awaitable))
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    auto await_suspend(std::coroutine_handle<> handle) {
        return m_writev_awaitable.await_suspend(handle);
    }

    std::expected<bool, WsError> await_resume() {
        auto writev_result = m_writev_awaitable.await_resume();
        if (!writev_result) {
            return std::unexpected(WsError(kWsSendError, writev_result.error().message()));
        }

        size_t bytes_written = writev_result.value();
        m_writer.updateRemainingWritev(bytes_written);

        if (m_writer.getRemainingBytes() == 0) {
            return true;
        }
        return false;
    }

private:
    WsWriterImpl<SocketType>& m_writer;
    WritevAwaitableType m_writev_awaitable;

public:
    std::expected<size_t, galay::kernel::IOError> m_result;
};

/**
 * @brief WebSocket帧发送等待体（send 版本 - 用于 SslSocket 等）
 */
template<typename SocketType>
class SendFrameAwaitableImpl : public galay::kernel::TimeoutSupport<SendFrameAwaitableImpl<SocketType>>
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
            return SendFrameWritevAwaitableImpl<SocketType>(*this, m_socket->writev(m_iovecs));
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
            return SendFrameWritevAwaitableImpl<SocketType>(*this, m_socket->writev(m_iovecs));
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
            return SendFrameWritevAwaitableImpl<SocketType>(*this, m_socket->writev(m_iovecs));
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
            return SendFrameWritevAwaitableImpl<SocketType>(*this, m_socket->writev(m_iovecs));
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
            return SendFrameWritevAwaitableImpl<SocketType>(*this, m_socket->writev(m_iovecs));
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
            return SendFrameWritevAwaitableImpl<SocketType>(*this, m_socket->writev(m_iovecs));
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
#include "galay-ssl/SslSocket.h"
namespace galay::websocket {
using SendFrameAwaitableSsl = SendFrameAwaitableImpl<galay::ssl::SslSocket>;
using WssWriter = WsWriterImpl<galay::ssl::SslSocket>;
} // namespace galay::websocket
#endif

#endif // GALAY_WS_WRITER_H
