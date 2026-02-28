#ifndef GALAY_HTTP2_CONNECTION_CORE_H
#define GALAY_HTTP2_CONNECTION_CORE_H

#include "galay-kernel/kernel/Coroutine.h"
#include <atomic>
#include <chrono>

namespace galay::http2
{

/**
 * @brief HTTP/2 连接级协议核心（重写阶段骨架）
 * @details 作为后续 connection-loop + frame-dispatch 的唯一承载对象。
 */
class Http2ConnectionCore
{
public:
    struct TimerConfig {
        std::chrono::milliseconds settings_ack_timeout{10000};
        std::chrono::milliseconds ping_interval{30000};
        std::chrono::milliseconds ping_timeout{10000};
        std::chrono::milliseconds graceful_shutdown_timeout{5000};
    };

    enum class TimerEvent {
        None,
        SendPing,
        SettingsAckTimeout,
        PingAckTimeout,
        GracefulShutdownTimeout
    };

    enum class State {
        Idle,
        Running,
        Stopped
    };

    Http2ConnectionCore() = default;

    State state() const noexcept { return m_state.load(std::memory_order_acquire); }
    bool stopRequested() const noexcept { return m_stop_requested.load(std::memory_order_acquire); }

    void requestStop() noexcept { m_stop_requested.store(true, std::memory_order_release); }

    void setTimerConfig(TimerConfig cfg) noexcept { m_timer_config = cfg; }
    const TimerConfig& timerConfig() const noexcept { return m_timer_config; }

    void markSettingsSent() noexcept {
        markSettingsSent(std::chrono::steady_clock::now());
    }
    void markSettingsSent(std::chrono::steady_clock::time_point at) noexcept {
        m_settings_ack_pending.store(true, std::memory_order_release);
        m_settings_sent_at = at;
    }
    void markSettingsAcked() noexcept { m_settings_ack_pending.store(false, std::memory_order_release); }
    bool isSettingsAckPending() const noexcept { return m_settings_ack_pending.load(std::memory_order_acquire); }

    void markFrameReceivedAt(std::chrono::steady_clock::time_point at) noexcept { m_last_frame_recv_at = at; }
    void markPingSent(std::chrono::steady_clock::time_point at) noexcept {
        m_waiting_ping_ack = true;
        m_last_ping_sent_at = at;
    }
    void markPingAcked() noexcept { m_waiting_ping_ack = false; }
    bool waitingPingAck() const noexcept { return m_waiting_ping_ack; }

    void beginGracefulShutdown(std::chrono::steady_clock::time_point at) noexcept {
        m_graceful_shutdown_started = true;
        m_graceful_shutdown_started_at = at;
    }

    TimerEvent checkTimers(std::chrono::steady_clock::time_point now) noexcept;

    galay::kernel::Coroutine run();

private:
    std::atomic<State> m_state{State::Idle};
    std::atomic<bool> m_stop_requested{false};
    std::atomic<bool> m_settings_ack_pending{false};
    TimerConfig m_timer_config{};
    std::chrono::steady_clock::time_point m_settings_sent_at{};
    std::chrono::steady_clock::time_point m_last_frame_recv_at{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point m_last_ping_sent_at{};
    bool m_waiting_ping_ack = false;
    bool m_graceful_shutdown_started = false;
    std::chrono::steady_clock::time_point m_graceful_shutdown_started_at{};
};

} // namespace galay::http2

#endif // GALAY_HTTP2_CONNECTION_CORE_H
