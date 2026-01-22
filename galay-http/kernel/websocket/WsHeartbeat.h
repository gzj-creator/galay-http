/**
 * @file WsHeartbeat.h
 * @brief WebSocket 心跳机制
 */

#ifndef GALAY_WS_HEARTBEAT_H
#define GALAY_WS_HEARTBEAT_H

#include "WsConn.h"
#include "galay-http/protoc/websocket/WebSocketBase.h"
#include "galay-kernel/kernel/Coroutine.h"
#include <chrono>
#include <atomic>

namespace galay::websocket
{

using namespace galay::kernel;

/**
 * @brief WebSocket 心跳配置
 */
struct WsHeartbeatConfig
{
    bool enabled = true;                                    // 是否启用心跳
    std::chrono::seconds ping_interval{30};                 // Ping 发送间隔
    std::chrono::seconds pong_timeout{10};                  // Pong 超时时间
    bool auto_close_on_timeout = true;                      // 超时是否自动关闭连接
};

/**
 * @brief WebSocket 心跳管理器
 * @details 自动发送 Ping 帧并检测 Pong 响应
 */
class WsHeartbeat
{
public:
    /**
     * @brief 构造函数
     * @param conn WebSocket 连接
     * @param config 心跳配置
     */
    WsHeartbeat(WsConn& conn, const WsHeartbeatConfig& config = WsHeartbeatConfig());

    /**
     * @brief 析构函数
     */
    ~WsHeartbeat() = default;

    /**
     * @brief 启动心跳协程
     * @return Coroutine
     * @details 定期发送 Ping 帧，检测连接是否存活
     */
    Coroutine start();

    /**
     * @brief 停止心跳
     */
    void stop();

    /**
     * @brief 通知收到 Pong 帧
     * @details 当收到 Pong 帧时调用此方法，重置超时计时器
     */
    void onPongReceived();

    /**
     * @brief 检查连接是否存活
     * @return 是否存活
     */
    bool isAlive() const;

    /**
     * @brief 获取最后一次 Pong 时间
     * @return 时间点
     */
    std::chrono::steady_clock::time_point getLastPongTime() const;

private:
    WsConn& m_conn;
    WsHeartbeatConfig m_config;
    std::atomic<bool> m_running;
    std::atomic<bool> m_alive;
    std::chrono::steady_clock::time_point m_last_pong_time;
    std::chrono::steady_clock::time_point m_last_ping_time;
};

} // namespace galay::websocket

#endif // GALAY_WS_HEARTBEAT_H
