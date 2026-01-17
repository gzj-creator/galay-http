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
class WsWriter;

/**
 * @brief WebSocket帧发送等待体
 *
 * @note 支持超时设置：
 * @code
 * auto result = co_await writer.sendText("hello").timeout(std::chrono::seconds(5));
 * @endcode
 */
class SendFrameAwaitable : public galay::kernel::TimeoutSupport<SendFrameAwaitable>
{
public:
    SendFrameAwaitable(WsWriter& writer, SendAwaitable&& send_awaitable)
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

    std::expected<size_t, WsError> await_resume();

private:
    WsWriter& m_writer;
    SendAwaitable m_send_awaitable;

public:
    // TimeoutSupport 需要访问此成员来设置超时错误
    std::expected<size_t, galay::kernel::IOError> m_result;
};

/**
 * @brief WebSocket写入器
 * @details 提供异步写入WebSocket帧的接口
 */
class WsWriter
{
public:
    WsWriter(const WsWriterSetting& setting, TcpSocket& socket)
        : m_setting(setting)
        , m_socket(socket)
        , m_remaining_bytes(0)
    {
    }

    /**
     * @brief 发送文本消息
     * @param text 文本内容
     * @param fin 是否是最后一个分片（默认true）
     * @return SendFrameAwaitable
     */
    SendFrameAwaitable sendText(const std::string& text, bool fin = true) {
        if (m_remaining_bytes == 0) {
            WsFrame frame = WsFrameParser::createTextFrame(text, fin);
            m_buffer = WsFrameParser::toBytes(frame, m_setting.use_mask);
            m_remaining_bytes = m_buffer.size();
        }

        size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
        const char* send_ptr = m_buffer.data() + sent_bytes;

        return SendFrameAwaitable(*this, m_socket.send(send_ptr, m_remaining_bytes));
    }

    /**
     * @brief 发送二进制消息
     * @param data 二进制数据
     * @param fin 是否是最后一个分片（默认true）
     * @return SendFrameAwaitable
     */
    SendFrameAwaitable sendBinary(const std::string& data, bool fin = true) {
        if (m_remaining_bytes == 0) {
            WsFrame frame = WsFrameParser::createBinaryFrame(data, fin);
            m_buffer = WsFrameParser::toBytes(frame, m_setting.use_mask);
            m_remaining_bytes = m_buffer.size();
        }

        size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
        const char* send_ptr = m_buffer.data() + sent_bytes;

        return SendFrameAwaitable(*this, m_socket.send(send_ptr, m_remaining_bytes));
    }

    /**
     * @brief 发送Ping帧
     * @param data Ping数据（可选）
     * @return SendFrameAwaitable
     */
    SendFrameAwaitable sendPing(const std::string& data = "") {
        if (m_remaining_bytes == 0) {
            WsFrame frame = WsFrameParser::createPingFrame(data);
            m_buffer = WsFrameParser::toBytes(frame, m_setting.use_mask);
            m_remaining_bytes = m_buffer.size();
        }

        size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
        const char* send_ptr = m_buffer.data() + sent_bytes;

        return SendFrameAwaitable(*this, m_socket.send(send_ptr, m_remaining_bytes));
    }

    /**
     * @brief 发送Pong帧
     * @param data Pong数据（通常是Ping的数据）
     * @return SendFrameAwaitable
     */
    SendFrameAwaitable sendPong(const std::string& data = "") {
        if (m_remaining_bytes == 0) {
            WsFrame frame = WsFrameParser::createPongFrame(data);
            m_buffer = WsFrameParser::toBytes(frame, m_setting.use_mask);
            m_remaining_bytes = m_buffer.size();
        }

        size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
        const char* send_ptr = m_buffer.data() + sent_bytes;

        return SendFrameAwaitable(*this, m_socket.send(send_ptr, m_remaining_bytes));
    }

    /**
     * @brief 发送Close帧
     * @param code 关闭状态码
     * @param reason 关闭原因
     * @return SendFrameAwaitable
     */
    SendFrameAwaitable sendClose(WsCloseCode code = WsCloseCode::Normal, const std::string& reason = "") {
        if (m_remaining_bytes == 0) {
            WsFrame frame = WsFrameParser::createCloseFrame(code, reason);
            m_buffer = WsFrameParser::toBytes(frame, m_setting.use_mask);
            m_remaining_bytes = m_buffer.size();
        }

        size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
        const char* send_ptr = m_buffer.data() + sent_bytes;

        return SendFrameAwaitable(*this, m_socket.send(send_ptr, m_remaining_bytes));
    }

    /**
     * @brief 发送自定义帧
     * @param frame WebSocket帧
     * @return SendFrameAwaitable
     */
    SendFrameAwaitable sendFrame(const WsFrame& frame) {
        if (m_remaining_bytes == 0) {
            m_buffer = WsFrameParser::toBytes(frame, m_setting.use_mask);
            m_remaining_bytes = m_buffer.size();
        }

        size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
        const char* send_ptr = m_buffer.data() + sent_bytes;

        return SendFrameAwaitable(*this, m_socket.send(send_ptr, m_remaining_bytes));
    }

    /**
     * @brief 更新剩余发送字节数
     */
    void updateRemaining(size_t bytes_sent) {
        if (bytes_sent >= m_remaining_bytes) {
            m_remaining_bytes = 0;
            m_buffer.clear();
        } else {
            m_remaining_bytes -= bytes_sent;
        }
    }

    /**
     * @brief 获取剩余发送字节数
     */
    size_t getRemainingBytes() const {
        return m_remaining_bytes;
    }

private:
    const WsWriterSetting& m_setting;
    TcpSocket& m_socket;
    std::string m_buffer;
    size_t m_remaining_bytes;
};

} // namespace galay::websocket

#endif // GALAY_WS_WRITER_H
