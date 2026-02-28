#include "Http2ConnectionCore.h"
#include "Http2FrameDispatcher.h"
#include "galay-kernel/common/Sleep.hpp"

namespace galay::http2
{

galay::kernel::Coroutine Http2ConnectionCore::run()
{
    H2DispatcherConnectionState dispatch_state;
    m_state.store(State::Running, std::memory_order_release);
    while (!m_stop_requested.load(std::memory_order_acquire)) {
        // Skeleton loop: concrete read/dispatch/schedule stages will be added in later tasks.
        // Keep dispatcher state alive so later tasks can wire in real frames.
        (void)dispatch_state;
        co_await galay::kernel::sleep(std::chrono::milliseconds(1));
    }
    m_state.store(State::Stopped, std::memory_order_release);
    co_return;
}

} // namespace galay::http2
