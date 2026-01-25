#ifndef GALAY_WS_READER_H
#define GALAY_WS_READER_H

#include "WsReaderSetting.h"
#include "galay-http/protoc/websocket/WebSocketFrame.h"
#include "galay-http/protoc/websocket/WebSocketError.h"
#include "galay-kernel/common/Buffer.h"
#include "galay-kernel/kernel/Awaitable.h"
#include "galay-kernel/kernel/Timeout.hpp"
#include "galay-kernel/async/TcpSocket.h"
#include <expected>
#include <coroutine>
#include <functional>

namespace galay::websocket
{

using namespace galay::kernel;
using namespace galay::async;

/**
 * @brief 控制帧回调函数类型
 * @param opcode 控制帧类型（Ping/Pong/Close）
 * @param payload 控制帧的负载数据
 */
using ControlFrameCallback = std::function<void(WsOpcode opcode, const std::string& payload)>;

/**
 * @brief WebSocket帧读取等待体
 *
 * @note 支持超时设置：
 * @code
 * auto result = co_await reader.getFrame(frame).timeout(std::chrono::seconds(5));
 * @endcode
 */
class GetFrameAwaitable : public galay::kernel::TimeoutSupport<GetFrameAwaitable>
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

public:
    // TimeoutSupport 需要访问此成员来设置超时错误
    std::expected<bool, galay::kernel::IOError> m_result;
};

/**
 * @brief WebSocket消息读取等待体
 * @details 自动处理分片消息，返回完整的消息内容
 *          控制帧（Ping/Pong/Close）会直接返回给用户，由用户决定如何处理
 *          用户需要根据 opcode 判断帧类型并自行响应（例如收到 Ping 时发送 Pong）
 *
 * @note 支持超时设置：
 * @code
 * auto result = co_await reader.getMessage(message, opcode).timeout(std::chrono::seconds(5));
 * @endcode
 */
class GetMessageAwaitable : public galay::kernel::TimeoutSupport<GetMessageAwaitable>
{
public:
    GetMessageAwaitable(RingBuffer& ring_buffer,
                       const WsReaderSetting& setting,
                       std::string& message,
                       WsOpcode& opcode,
                       ReadvAwaitable&& readv_awaitable,
                       bool is_server,
                       TcpSocket& socket,
                       bool use_mask,
                       ControlFrameCallback control_frame_callback = nullptr)
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_message(message)
        , m_opcode(opcode)
        , m_readv_awaitable(std::move(readv_awaitable))
        , m_is_server(is_server)
        , m_socket(socket)
        , m_use_mask(use_mask)
        , m_total_received(0)
        , m_first_frame(true)
        , m_control_frame_callback(control_frame_callback)
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
    TcpSocket& m_socket;
    bool m_use_mask;
    size_t m_total_received;
    bool m_first_frame;
    ControlFrameCallback m_control_frame_callback;

public:
    // TimeoutSupport 需要访问此成员来设置超时错误
    std::expected<bool, galay::kernel::IOError> m_result;
};

/**
 * @brief WebSocket读取器
 * @details 提供异步读取WebSocket帧和消息的接口
 *          控制帧（Ping/Pong/Close）会返回给用户，由用户自行处理
 */
class WsReader
{
public:
    WsReader(RingBuffer& ring_buffer, const WsReaderSetting& setting, TcpSocket& socket, bool is_server = true, bool use_mask = false)
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_socket(socket)
        , m_is_server(is_server)
        , m_use_mask(use_mask)
        , m_control_frame_callback(nullptr)
    {
    }

    /**
     * @brief 设置控制帧回调函数
     * @param callback 控制帧回调函数
     * @details 当收到 Ping/Pong/Close 帧时会调用此回调
     *          注意：控制帧不会自动响应，用户需要自行处理
     */
    void setControlFrameCallback(ControlFrameCallback callback) {
        m_control_frame_callback = callback;
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
     * @param opcode 用于存储消息类型的WsOpcode引用（包括控制帧：Ping/Pong/Close）
     * @return GetMessageAwaitable 消息等待体
     * @note 用户需要根据 opcode 判断消息类型：
     *       - Text/Binary: 数据消息
     *       - Ping: 需要用 writer.sendPong() 响应
     *       - Pong: 心跳响应
     *       - Close: 连接关闭请求
     */
    GetMessageAwaitable getMessage(std::string& message, WsOpcode& opcode) {
        return GetMessageAwaitable(m_ring_buffer, m_setting, message, opcode,
                                  m_socket.readv(m_ring_buffer.getWriteIovecs()),
                                  m_is_server, m_socket, m_use_mask, m_control_frame_callback);
    }

private:
    RingBuffer& m_ring_buffer;
    const WsReaderSetting& m_setting;
    TcpSocket& m_socket;
    bool m_is_server;
    bool m_use_mask;
    ControlFrameCallback m_control_frame_callback;
};

} // namespace galay::websocket

#endif // GALAY_WS_READER_H
