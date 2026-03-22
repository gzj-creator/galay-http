#include "galay-kernel/kernel/Runtime.h"
#include "galay-kernel/kernel/Coroutine.h"
#include "galay-kernel/common/Sleep.hpp"
#include "galay-kernel/concurrency/UnsafeChannel.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

std::atomic<int> g_result{0};

Coroutine receiver(UnsafeChannel<int>* channel) {
    auto value = co_await channel->recv();
    if (!value) {
        std::cerr << "receiver failed: " << value.error().message() << "\n";
        g_result.store(-1, std::memory_order_release);
        co_return;
    }

    if (value.value() != 42) {
        std::cerr << "unexpected payload: " << value.value() << "\n";
        g_result.store(-2, std::memory_order_release);
        co_return;
    }

    g_result.store(1, std::memory_order_release);
    co_return;
}

Coroutine sender(UnsafeChannel<int>* channel) {
    co_await galay::kernel::sleep(50ms);
    channel->send(42);
    co_return;
}

Coroutine runScenario() {
    UnsafeChannel<int> channel;
    Coroutine waiting = receiver(&channel);

    co_await spawn(waiting);
    co_await spawn(sender(&channel));

    // Regression target:
    // `wait()` must join an already spawned coroutine instead of re-scheduling it.
    co_await waiting.wait();
    co_return;
}

}  // namespace

int main() {
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (scheduler == nullptr) {
        std::cerr << "No IO scheduler available\n";
        runtime.stop();
        return 1;
    }

    scheduleCoroutine(scheduler, runScenario());

    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (g_result.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }

    runtime.stop();

    const int result = g_result.load(std::memory_order_acquire);
    if (result != 1) {
        std::cerr << "T63-CoroutineWaitJoin FAIL result=" << result << "\n";
        return 1;
    }

    std::cout << "T63-CoroutineWaitJoin PASS\n";
    return 0;
}
