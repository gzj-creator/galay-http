#ifndef GALAY_WS_READER_H
#define GALAY_WS_READER_H

#include "WsReaderSetting.h"
#include "galay-http/protoc/websocket/WebSocketFrame.h"
#include "galay-http/protoc/websocket/WebSocketError.h"
#include "galay-kernel/common/Buffer.h"
#include "galay-kernel/kernel/Awaitable.h"
#include "galay-kernel/async/TcpSocket.h"
#include <expected>
#include <coroutine>

namespace galay::websocket
{

using namespace galay::kernel;
using namespace galay::async;

/**
 * @brief WebSocket帧读取等待体
 */
class GetFrameAwaitable
{
public:
    GetFrameAwaitable(RingBuffer& ring_buffer,
                     const WsReaderSetting& setting,
                     WsFrame& frame,
                     ReadvAwaitable&& readv_awaitable,
                     bool is_server)
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_frame(frame)
        , m_readv_awaitable(std::move(readv_awaitable))
        , m_is_server(is_server)
        , m_total_received(0)
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    auto await_suspend(std::coroutine_handle<> handle) {
        return m_readv_awaitable.await_suspend(handle);
    }

    /**
     * @brief 恢复协程时调用
     * @return std::expected<bool, WsError>
     *         - true: 帧完整解析
     *         - false: 不完整，需要继续调用
     *         - WsError: 解析错误
     */
    std::expected<bool, WsError> await_resume();

private:
    RingBuffer& m_ring_buffer;
    const WsReaderSetting& m_setting;
    WsFrame& m_frame;
    ReadvAwaitable m_readv_awaitable;
    bool m_is_server;
    size_t m_total_received;
};

/**
 * @brief WebSocket消息读取等待体
 * @details 自动处理分片消息，返回完整的消息内容
 */
class GetMessageAwaitable
{
public:
    GetMessageAwaitable(RingBuffer& ring_buffer,
                       const WsReaderSetting& setting,
                       std::string& message,
                       WsOpcode& opcode,
                       ReadvAwaitable&& readv_awaitable,
                       bool is_server)
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_message(message)
        , m_opcode(opcode)
        , m_readv_awaitable(std::move(readv_awaitable))
        , m_is_server(is_server)
        , m_total_received(0)
        , m_first_frame(true)
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    auto await_suspend(std::coroutine_handle<> handle) {
        return m_readv_awaitable.await_suspend(handle);
    }

    /**
     * @brief 恢复协程时调用
     * @return std::expected<bool, WsError>
     *         - true: 消息完整接收（FIN=1）
     *         - false: 不完整，需要继续调用
     *         - WsError: 解析错误
     */
    std::expected<bool, WsError> await_resume();

private:
    RingBuffer& m_ring_buffer;
    const WsReaderSetting& m_setting;
    std::string& m_message;
    WsOpcode& m_opcode;
    ReadvAwaitable m_readv_awaitable;
    bool m_is_server;
    size_t m_total_received;
    bool m_first_frame;
};

/**
 * @brief WebSocket读取器
 * @details 提供异步读取WebSocket帧和消息的接口
 */
class WsReader
{
public:
    WsReader(RingBuffer& ring_buffer, const WsReaderSetting& setting, TcpSocket& socket, bool is_server = true)
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_socket(socket)
        , m_is_server(is_server)
    {
    }

    /**
     * @brief 获取一个完整的WebSocket帧
     * @param frame WsFrame引用，用于存储解析结果
     * @return GetFrameAwaitable 帧等待体
     */
    GetFrameAwaitable getFrame(WsFrame& frame) {
        return GetFrameAwaitable(m_ring_buffer, m_setting, frame,
                                m_socket.readv(m_ring_buffer.getWriteIovecs()),
                                m_is_server);
    }

    /**
     * @brief 获取一个完整的WebSocket消息（自动处理分片）
     * @param message 用于存储消息内容的string引用
     * @param opcode 用于存储消息类型的WsOpcode引用
     * @return GetMessageAwaitable 消息等待体
     */
    GetMessageAwaitable getMessage(std::string& message, WsOpcode& opcode) {
        return GetMessageAwaitable(m_ring_buffer, m_setting, message, opcode,
                                  m_socket.readv(m_ring_buffer.getWriteIovecs()),
                                  m_is_server);
    }

private:
    RingBuffer& m_ring_buffer;
    const WsReaderSetting& m_setting;
    TcpSocket& m_socket;
    bool m_is_server;
};

} // namespace galay::websocket

#endif // GALAY_WS_READER_H
