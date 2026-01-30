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
#include <string>

namespace galay::websocket
{

using namespace galay::kernel;
using namespace galay::async;

// 前向声明
template<typename SocketType>
class WsWriterImpl;

/**
 * @brief WebSocket帧发送等待体模板类
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

    std::expected<size_t, WsError> await_resume() {
        auto send_result = m_send_awaitable.await_resume();
        if (!send_result) {
            return std::unexpected(WsError(kWsSendError, send_result.error().message()));
        }

        size_t bytes_written = send_result.value();
        m_writer.updateRemaining(bytes_written);

        return bytes_written;
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
        , m_socket(socket)
        , m_remaining_bytes(0)
    {
    }

    SendFrameAwaitableImpl<SocketType> sendText(const std::string& text, bool fin = true) {
        if (m_remaining_bytes == 0) {
            WsFrame frame = WsFrameParser::createTextFrame(text, fin);
            m_buffer = WsFrameParser::toBytes(frame, m_setting.use_mask);
            m_remaining_bytes = m_buffer.size();
        }

        size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
        const char* send_ptr = m_buffer.data() + sent_bytes;

        return SendFrameAwaitableImpl<SocketType>(*this, m_socket.send(send_ptr, m_remaining_bytes));
    }

    SendFrameAwaitableImpl<SocketType> sendBinary(const std::string& data, bool fin = true) {
        if (m_remaining_bytes == 0) {
            WsFrame frame = WsFrameParser::createBinaryFrame(data, fin);
            m_buffer = WsFrameParser::toBytes(frame, m_setting.use_mask);
            m_remaining_bytes = m_buffer.size();
        }

        size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
        const char* send_ptr = m_buffer.data() + sent_bytes;

        return SendFrameAwaitableImpl<SocketType>(*this, m_socket.send(send_ptr, m_remaining_bytes));
    }

    SendFrameAwaitableImpl<SocketType> sendPing(const std::string& data = "") {
        if (m_remaining_bytes == 0) {
            WsFrame frame = WsFrameParser::createPingFrame(data);
            m_buffer = WsFrameParser::toBytes(frame, m_setting.use_mask);
            m_remaining_bytes = m_buffer.size();
        }

        size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
        const char* send_ptr = m_buffer.data() + sent_bytes;

        return SendFrameAwaitableImpl<SocketType>(*this, m_socket.send(send_ptr, m_remaining_bytes));
    }

    SendFrameAwaitableImpl<SocketType> sendPong(const std::string& data = "") {
        if (m_remaining_bytes == 0) {
            WsFrame frame = WsFrameParser::createPongFrame(data);
            m_buffer = WsFrameParser::toBytes(frame, m_setting.use_mask);
            m_remaining_bytes = m_buffer.size();
        }

        size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
        const char* send_ptr = m_buffer.data() + sent_bytes;

        return SendFrameAwaitableImpl<SocketType>(*this, m_socket.send(send_ptr, m_remaining_bytes));
    }

    SendFrameAwaitableImpl<SocketType> sendClose(WsCloseCode code = WsCloseCode::Normal, const std::string& reason = "") {
        if (m_remaining_bytes == 0) {
            WsFrame frame = WsFrameParser::createCloseFrame(code, reason);
            m_buffer = WsFrameParser::toBytes(frame, m_setting.use_mask);
            m_remaining_bytes = m_buffer.size();
        }

        size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
        const char* send_ptr = m_buffer.data() + sent_bytes;

        return SendFrameAwaitableImpl<SocketType>(*this, m_socket.send(send_ptr, m_remaining_bytes));
    }

    SendFrameAwaitableImpl<SocketType> sendFrame(const WsFrame& frame) {
        if (m_remaining_bytes == 0) {
            m_buffer = WsFrameParser::toBytes(frame, m_setting.use_mask);
            m_remaining_bytes = m_buffer.size();
        }

        size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
        const char* send_ptr = m_buffer.data() + sent_bytes;

        return SendFrameAwaitableImpl<SocketType>(*this, m_socket.send(send_ptr, m_remaining_bytes));
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
    const WsWriterSetting& m_setting;
    SocketType& m_socket;
    std::string m_buffer;
    size_t m_remaining_bytes;
};

// 类型别名 - WebSocket over TCP
using SendFrameAwaitable = SendFrameAwaitableImpl<TcpSocket>;
using WsWriter = WsWriterImpl<TcpSocket>;

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-socket/async/SslSocket.h"
using SendFrameAwaitableSsl = SendFrameAwaitableImpl<galay::async::SslSocket>;
using WssWriter = WsWriterImpl<galay::async::SslSocket>;
#endif

} // namespace galay::websocket

#endif // GALAY_WS_WRITER_H
