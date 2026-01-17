/**
 * @file test_timeout_simple.cc
 * @brief 简单的超时验证测试
 * @details 验证超时机制是否正确工作
 */

#include "galay-http/kernel/http/HttpClient.h"
#include "galay-kernel/kernel/Runtime.h"
#include "galay-kernel/common/Log.h"
#include <iostream>
#include <chrono>

using namespace galay::http;
using namespace galay::kernel;
using namespace galay::async;
using namespace std::chrono_literals;

/**
 * @brief 测试：连接超时验证
 * @details 尝试连接到不可达的地址，验证超时是否正确触发
 */
Coroutine testConnectTimeout(IOScheduler* scheduler)
{
    LogInfo("=== Test: Connect Timeout Verification ===");

    // 使用一个不可路由的 IP 地址（TEST-NET-1，RFC 5737）
    // 这个地址保证不会有响应，适合测试超时
    const char* unreachable_ip = "192.0.2.1";
    const uint16_t unreachable_port = 9999;

    TcpSocket socket(IPType::IPV4);
    auto nonblock_result = socket.option().handleNonBlock();
    if (!nonblock_result) {
        LogError("Failed to set non-block");
        co_return;
    }

    LogInfo("Attempting to connect to {}:{} with 2s timeout...", unreachable_ip, unreachable_port);

    auto start_time = std::chrono::steady_clock::now();
    Host host(IPType::IPV4, unreachable_ip, unreachable_port);
    auto result = co_await socket.connect(host).timeout(2000ms);
    auto end_time = std::chrono::steady_clock::now();

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    LogInfo("Connect attempt finished after {} ms", elapsed);

    if (!result) {
        LogInfo("Connect failed as expected: {}", result.error().message());
        LogInfo("Error code: {}", result.error().code());

        // 验证是否是超时错误
        if (result.error().code() == kTimeout) {
            LogInfo("✓ Timeout error detected correctly!");

            // 验证超时时间是否合理（应该接近 2000ms）
            if (elapsed >= 1800 && elapsed <= 2500) {
                LogInfo("✓ Timeout duration is correct: {} ms (expected ~2000ms)", elapsed);
            } else {
                LogError("❌ Timeout duration is incorrect: {} ms (expected ~2000ms)", elapsed);
            }
        } else {
            LogError("❌ Expected timeout error, but got error code: {}", result.error().code());
        }
    } else {
        LogError("❌ Connect should have timed out but succeeded!");
    }

    LogInfo("");
    co_return;
}

/**
 * @brief 测试：正常连接（无超时）
 * @details 连接到本地回环地址，验证正常连接不会触发超时
 */
Coroutine testNormalConnect(IOScheduler* scheduler)
{
    LogInfo("=== Test: Normal Connect (No Timeout) ===");

    // 尝试连接到本地回环地址的一个端口
    // 即使端口没有监听，连接也会快速失败（不会超时）
    TcpSocket socket(IPType::IPV4);
    auto nonblock_result = socket.option().handleNonBlock();
    if (!nonblock_result) {
        LogError("Failed to set non-block");
        co_return;
    }

    LogInfo("Attempting to connect to 127.0.0.1:9999 with 5s timeout...");

    auto start_time = std::chrono::steady_clock::now();
    Host host(IPType::IPV4, "127.0.0.1", 9999);
    auto result = co_await socket.connect(host).timeout(5000ms);
    auto end_time = std::chrono::steady_clock::now();

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    LogInfo("Connect attempt finished after {} ms", elapsed);

    if (!result) {
        LogInfo("Connect failed: {}", result.error().message());
        LogInfo("Error code: {}", result.error().code());

        // 连接到本地回环地址应该快速失败（不是超时）
        if (result.error().code() != kTimeout) {
            LogInfo("✓ Failed quickly without timeout (as expected for localhost)");

            if (elapsed < 1000) {
                LogInfo("✓ Failed quickly: {} ms", elapsed);
            } else {
                LogInfo("⚠ Took longer than expected: {} ms", elapsed);
            }
        } else {
            LogError("❌ Should not timeout when connecting to localhost");
        }
    } else {
        LogInfo("⚠ Connect succeeded (port might be open)");
    }

    LogInfo("");
    co_return;
}

/**
 * @brief 测试：HttpClient 请求超时
 * @details 如果有测试服务器运行，测试 HTTP 请求超时
 */
Coroutine testHttpRequestTimeout(IOScheduler* scheduler)
{
    LogInfo("=== Test: HTTP Request Timeout ===");

    TcpSocket socket(IPType::IPV4);
    auto nonblock_result = socket.option().handleNonBlock();
    if (!nonblock_result) {
        LogError("Failed to set non-block");
        co_return;
    }

    LogInfo("Attempting to connect to 127.0.0.1:8080...");
    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);

    if (!connect_result) {
        LogInfo("⚠ Cannot connect to test server (this is OK if no server is running)");
        LogInfo("  Error: {}", connect_result.error().message());
        co_return;
    }

    LogInfo("✓ Connected to test server");

    // 创建 HttpClient
    HttpClient client(std::move(socket));

    // 发送请求并设置 1 秒超时
    LogInfo("Sending GET request with 1s timeout...");

    auto start_time = std::chrono::steady_clock::now();
    int loop_count = 0;

    while (true) {
        loop_count++;
        auto result = co_await client.get("/delay/5").timeout(1000ms);

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time).count();

        if (!result) {
            LogInfo("Request failed after {} ms (loop {})", elapsed, loop_count);
            LogInfo("Error: {}", result.error().message());
            LogInfo("Error code: {}", result.error().code());

            if (result.error().code() == kRequestTimeOut || result.error().code() == kRecvTimeOut) {
                LogInfo("✓ Request timed out as expected!");

                if (elapsed >= 900 && elapsed <= 1500) {
                    LogInfo("✓ Timeout duration is correct: {} ms (expected ~1000ms)", elapsed);
                } else {
                    LogInfo("⚠ Timeout duration: {} ms (expected ~1000ms)", elapsed);
                }
            } else {
                LogInfo("⚠ Got error but not timeout: {}", result.error().code());
            }
            break;
        } else if (result.value().has_value()) {
            LogInfo("⚠ Request completed (server might not support /delay/5)");
            break;
        }

        // 继续循环
        if (loop_count > 1000) {
            LogError("❌ Too many loops, breaking");
            break;
        }
    }

    co_await client.close();
    LogInfo("");
    co_return;
}

/**
 * @brief 测试：多个不同的超时时间
 * @details 验证不同的超时时间设置是否都能正确工作
 */
Coroutine testMultipleTimeouts(IOScheduler* scheduler)
{
    LogInfo("=== Test: Multiple Timeout Durations ===");

    const std::vector<int> timeout_durations = {500, 1000, 2000};

    for (int timeout_ms : timeout_durations) {
        LogInfo("Testing {}ms timeout...", timeout_ms);

        TcpSocket socket(IPType::IPV4);
        auto nonblock_result = socket.option().handleNonBlock();
        if (!nonblock_result) {
            LogError("Failed to set non-block");
            continue;
        }

        auto start_time = std::chrono::steady_clock::now();
        Host host(IPType::IPV4, "192.0.2.1", 9999);
        auto result = co_await socket.connect(host).timeout(std::chrono::milliseconds(timeout_ms));
        auto end_time = std::chrono::steady_clock::now();

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

        LogInfo("  Elapsed: {} ms", elapsed);

        if (!result && result.error().code() == kTimeout) {
            // 允许 ±20% 的误差
            int min_expected = timeout_ms * 0.8;
            int max_expected = timeout_ms * 1.2;

            if (elapsed >= min_expected && elapsed <= max_expected) {
                LogInfo("  ✓ Timeout duration correct");
            } else {
                LogInfo("  ⚠ Timeout duration off: {} ms (expected ~{}ms)", elapsed, timeout_ms);
            }
        } else {
            LogInfo("  ⚠ Unexpected result");
        }
    }

    LogInfo("");
    co_return;
}

int main()
{
    LogInfo("==================================");
    LogInfo("Timeout Verification Tests");
    LogInfo("==================================\n");

    try {
        Runtime runtime;
        runtime.start();

        LogInfo("Runtime started with {} IO schedulers\n", runtime.getIOSchedulerCount());

        auto* scheduler = runtime.getNextIOScheduler();
        if (!scheduler) {
            LogError("No IO scheduler available");
            return 1;
        }

        // 运行测试
        scheduler->spawn(testConnectTimeout(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        scheduler->spawn(testNormalConnect(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        scheduler->spawn(testMultipleTimeouts(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(8));

        scheduler->spawn(testHttpRequestTimeout(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        runtime.stop();

        LogInfo("==================================");
        LogInfo("All Tests Completed");
        LogInfo("==================================");

    } catch (const std::exception& e) {
        LogError("Fatal error: {}", e.what());
        return 1;
    }

    return 0;
}
