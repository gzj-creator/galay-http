/**
 * @file WsHeartbeat.cc
 * @brief WebSocket 心跳机制实现
 */

#include "WsHeartbeat.h"
#include "galay-kernel/common/Log.h"
#include "galay-kernel/common/Sleep.hpp"

namespace galay::websocket
{

WsHeartbeat::WsHeartbeat(WsConn& conn, const WsHeartbeatConfig& config)
    : m_conn(conn)
    , m_config(config)
    , m_running(false)
    , m_alive(true)
    , m_last_pong_time(std::chrono::steady_clock::now())
    , m_last_ping_time(std::chrono::steady_clock::now())
{
}

Coroutine WsHeartbeat::start()
{
    if (!m_config.enabled) {
        co_return;
    }

    m_running.store(true);
    m_alive.store(true);
    m_last_pong_time = std::chrono::steady_clock::now();

    LogInfo("WebSocket heartbeat started: ping_interval={}s, pong_timeout={}s",
            m_config.ping_interval.count(),
            m_config.pong_timeout.count());

    while (m_running.load()) {
        // 等待 ping_interval（使用协程友好的 sleep）
        co_await kernel::sleep(m_config.ping_interval);

        if (!m_running.load()) {
            break;
        }

        // 检查上次 Pong 是否超时
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - m_last_pong_time);

        if (elapsed > m_config.pong_timeout + m_config.ping_interval) {
            // Pong 超时
            LogWarn("WebSocket Pong timeout: {}s elapsed since last pong",
                    elapsed.count());

            m_alive.store(false);

            if (m_config.auto_close_on_timeout) {
                LogInfo("Auto closing connection due to heartbeat timeout");
                co_await m_conn.close();
                break;
            }
        }

        // 发送 Ping 帧
        WsFrame ping_frame;
        ping_frame.header.fin = true;
        ping_frame.header.opcode = WsOpcode::Ping;
        ping_frame.header.payload_length = 0;

        auto writer = m_conn.getWriter();
        auto result = co_await writer.sendFrame(ping_frame);

        if (!result) {
            LogError("Failed to send Ping frame: {}", result.error().message());
            m_alive.store(false);
            break;
        }

        m_last_ping_time = std::chrono::steady_clock::now();
        LogDebug("Ping frame sent");
    }

    LogInfo("WebSocket heartbeat stopped");
    co_return;
}

void WsHeartbeat::stop()
{
    m_running.store(false);
}

void WsHeartbeat::onPongReceived()
{
    m_last_pong_time = std::chrono::steady_clock::now();
    m_alive.store(true);
    LogDebug("Pong frame received, connection is alive");
}

bool WsHeartbeat::isAlive() const
{
    return m_alive.load();
}

std::chrono::steady_clock::time_point WsHeartbeat::getLastPongTime() const
{
    return m_last_pong_time;
}

} // namespace galay::websocket
