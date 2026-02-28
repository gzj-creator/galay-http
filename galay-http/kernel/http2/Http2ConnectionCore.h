#ifndef GALAY_HTTP2_CONNECTION_CORE_H
#define GALAY_HTTP2_CONNECTION_CORE_H

#include "galay-kernel/kernel/Coroutine.h"
#include <atomic>

namespace galay::http2
{

/**
 * @brief HTTP/2 连接级协议核心（重写阶段骨架）
 * @details 作为后续 connection-loop + frame-dispatch 的唯一承载对象。
 */
class Http2ConnectionCore
{
public:
    enum class State {
        Idle,
        Running,
        Stopped
    };

    Http2ConnectionCore() = default;

    State state() const noexcept { return m_state.load(std::memory_order_acquire); }
    bool stopRequested() const noexcept { return m_stop_requested.load(std::memory_order_acquire); }

    void requestStop() noexcept { m_stop_requested.store(true, std::memory_order_release); }

    void markSettingsSent() noexcept { m_settings_ack_pending.store(true, std::memory_order_release); }
    void markSettingsAcked() noexcept { m_settings_ack_pending.store(false, std::memory_order_release); }
    bool isSettingsAckPending() const noexcept { return m_settings_ack_pending.load(std::memory_order_acquire); }

    galay::kernel::Coroutine run();

private:
    std::atomic<State> m_state{State::Idle};
    std::atomic<bool> m_stop_requested{false};
    std::atomic<bool> m_settings_ack_pending{false};
};

} // namespace galay::http2

#endif // GALAY_HTTP2_CONNECTION_CORE_H
