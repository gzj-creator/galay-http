/**
 * @file test_http_client_timeout.cc
 * @brief HttpClient 超时和断连测试
 * @details 测试 HttpClientAwaitable 的超时功能和连接断开处理
 */

#include "galay-http/kernel/http/HttpClient.h"
#include "galay-kernel/kernel/Runtime.h"
#include "galay-kernel/common/Log.h"
#include <iostream>
#include <chrono>
#include <thread>

using namespace galay::http;
using namespace galay::kernel;
using namespace galay::async;
using namespace std::chrono_literals;

// 测试服务器配置
constexpr const char* TEST_HOST = "127.0.0.1";
constexpr uint16_t TEST_PORT = 8080;

/**
 * @brief 测试：请求超时
 * @details 服务器延迟响应，客户端设置较短超时时间
 */
Coroutine testRequestTimeout(IOScheduler* scheduler)
{
    LogInfo("=== Test: Request Timeout ===");

    try {
        // 创建socket并连接
        TcpSocket socket(IPType::IPV4);
        auto nonblock_result = socket.option().handleNonBlock();
        if (!nonblock_result) {
            LogError("Failed to set non-block");
            co_return;
        }

        Host host(IPType::IPV4, TEST_HOST, TEST_PORT);
        auto connectResult = co_await socket.connect(host);

        if (!connectResult) {
            LogError("❌ Connect failed: {}", connectResult.error().message());
            co_return;
        }
        LogInfo("✓ Connected to server");

        // 创建 HttpClient
        HttpClient client(std::move(socket));

        // 发送请求并设置 1 秒超时（假设服务器会延迟 5 秒响应）
        LogInfo("Sending GET request with 1s timeout...");

        int loop_count = 0;
        while (true) {
            loop_count++;
            auto result = co_await client.get("/delay/5").timeout(1000ms);

            if (!result) {
                // 期望超时错误
                if (result.error().code() == kRequestTimeOut || result.error().code() == kRecvTimeOut) {
                    LogInfo("✓ Request timed out as expected: {}", result.error().message());
                } else {
                    LogError("❌ Unexpected error: {}", result.error().message());
                }
                break;
            } else if (result.value().has_value()) {
                LogError("❌ Request should have timed out but succeeded");
                break;
            }
            // std::nullopt，继续循环
            LogInfo("  Request in progress (loop {})...", loop_count);
        }

        // 关闭连接
        co_await client.close();
        LogInfo("✓ Connection closed\n");

    } catch (const std::exception& e) {
        LogError("❌ Exception: {}", e.what());
    }
    co_return;
}

/**
 * @brief 测试：连接超时
 * @details 连接到不存在的服务器，测试连接超时
 */
Coroutine testConnectTimeout(IOScheduler* scheduler)
{
    LogInfo("=== Test: Connect Timeout ===");

    try {
        // 尝试连接到不存在的服务器（使用不可路由的 IP）
        TcpSocket socket(IPType::IPV4);
        auto nonblock_result = socket.option().handleNonBlock();
        if (!nonblock_result) {
            LogError("Failed to set non-block");
            co_return;
        }

        LogInfo("Attempting to connect to unreachable host with 2s timeout...");
        Host host(IPType::IPV4, "192.0.2.1", 9999);
        auto connectResult = co_await socket.connect(host).timeout(2000ms);

        if (!connectResult) {
            if (connectResult.error().code() == kTimeout) {
                LogInfo("✓ Connect timed out as expected: {}", connectResult.error().message());
            } else {
                LogInfo("⚠ Connect failed with error: {}", connectResult.error().message());
            }
        } else {
            LogError("❌ Connect should have timed out but succeeded");
        }

    } catch (const std::exception& e) {
        LogError("❌ Exception: {}", e.what());
    }

    LogInfo("");
    co_return;
}

/**
 * @brief 测试：服务器主动断开连接
 * @details 服务器在发送部分数据后断开连接
 */
Coroutine testServerDisconnect(IOScheduler* scheduler)
{
    LogInfo("=== Test: Server Disconnect ===");

    try {
        // 创建socket并连接
        TcpSocket socket(IPType::IPV4);
        auto nonblock_result = socket.option().handleNonBlock();
        if (!nonblock_result) {
            LogError("Failed to set non-block");
            co_return;
        }

        Host host(IPType::IPV4, TEST_HOST, TEST_PORT);
        auto connectResult = co_await socket.connect(host);

        if (!connectResult) {
            LogError("❌ Connect failed: {}", connectResult.error().message());
            co_return;
        }
        LogInfo("✓ Connected to server");

        // 创建 HttpClient
        HttpClient client(std::move(socket));

        // 请求一个会导致服务器断开连接的端点
        LogInfo("Sending GET request to /disconnect endpoint...");

        int loop_count = 0;
        while (true) {
            loop_count++;
            auto result = co_await client.get("/disconnect");

            if (!result) {
                // 期望连接错误
                LogInfo("✓ Detected server disconnect: {}", result.error().message());
                break;
            } else if (result.value().has_value()) {
                LogError("❌ Request should have failed but succeeded");
                break;
            }
            // std::nullopt，继续循环
            LogInfo("  Request in progress (loop {})...", loop_count);
        }

        // 尝试关闭连接（可能已经断开）
        auto closeResult = co_await client.close();
        if (closeResult) {
            LogInfo("✓ Connection closed");
        } else {
            LogInfo("⚠ Close failed (connection may already be closed): {}",
                    closeResult.error().message());
        }

    } catch (const std::exception& e) {
        LogError("❌ Exception: {}", e.what());
    }

    LogInfo("");
    co_return;
}

/**
 * @brief 测试：接收超时
 * @details 服务器发送部分数据后停止，测试接收超时
 */
Coroutine testReceiveTimeout(IOScheduler* scheduler)
{
    LogInfo("=== Test: Receive Timeout ===");

    try {
        // 创建socket并连接
        TcpSocket socket(IPType::IPV4);
        auto nonblock_result = socket.option().handleNonBlock();
        if (!nonblock_result) {
            LogError("Failed to set non-block");
            co_return;
        }

        Host host(IPType::IPV4, TEST_HOST, TEST_PORT);
        auto connectResult = co_await socket.connect(host);

        if (!connectResult) {
            LogError("❌ Connect failed: {}", connectResult.error().message());
            co_return;
        }
        LogInfo("✓ Connected to server");

        // 创建 HttpClient
        HttpClient client(std::move(socket));

        // 请求一个会发送部分数据然后停止的端点
        LogInfo("Sending GET request to /partial endpoint with 2s timeout...");

        int loop_count = 0;
        while (true) {
            loop_count++;
            auto result = co_await client.get("/partial").timeout(2000ms);

            if (!result) {
                if (result.error().code() == kRequestTimeOut || result.error().code() == kRecvTimeOut) {
                    LogInfo("✓ Receive timed out as expected: {}", result.error().message());
                } else {
                    LogInfo("⚠ Request failed with error: {}", result.error().message());
                }
                break;
            } else if (result.value().has_value()) {
                LogError("❌ Request should have timed out but succeeded");
                break;
            }
            // std::nullopt，继续循环
            LogInfo("  Request in progress (loop {})...", loop_count);
        }

        // 关闭连接
        co_await client.close();
        LogInfo("✓ Connection closed\n");

    } catch (const std::exception& e) {
        LogError("❌ Exception: {}", e.what());
    }
    co_return;
}

/**
 * @brief 测试：多次超时重试
 * @details 测试超时后重新发起请求
 */
Coroutine testTimeoutRetry(IOScheduler* scheduler)
{
    LogInfo("=== Test: Timeout Retry ===");

    try {
        // 创建socket并连接
        TcpSocket socket(IPType::IPV4);
        auto nonblock_result = socket.option().handleNonBlock();
        if (!nonblock_result) {
            LogError("Failed to set non-block");
            co_return;
        }

        Host host(IPType::IPV4, TEST_HOST, TEST_PORT);
        auto connectResult = co_await socket.connect(host);

        if (!connectResult) {
            LogError("❌ Connect failed: {}", connectResult.error().message());
            co_return;
        }
        LogInfo("✓ Connected to server");

        // 创建 HttpClient
        HttpClient client(std::move(socket));

        // 第一次请求：超时
        LogInfo("First request with 1s timeout...");
        int loop_count = 0;
        while (true) {
            loop_count++;
            auto result1 = co_await client.get("/delay/5").timeout(1000ms);

            if (!result1) {
                if (result1.error().code() == kRequestTimeOut || result1.error().code() == kRecvTimeOut) {
                    LogInfo("✓ First request timed out as expected");
                } else {
                    LogInfo("⚠ First request failed: {}", result1.error().message());
                }
                break;
            } else if (result1.value().has_value()) {
                LogInfo("⚠ First request did not timeout as expected");
                break;
            }
            LogInfo("  First request in progress (loop {})...", loop_count);
        }

        // 第二次请求：正常完成
        LogInfo("Second request with sufficient timeout...");
        loop_count = 0;
        while (true) {
            loop_count++;
            auto result2 = co_await client.get("/api/data").timeout(5000ms);

            if (!result2) {
                LogInfo("⚠ Second request failed: {}", result2.error().message());
                break;
            } else if (result2.value().has_value()) {
                auto& response = result2.value().value();
                LogInfo("✓ Second request succeeded");
                LogInfo("  Status: {}", static_cast<int>(response.header().code()));
                LogInfo("  Total loops: {}", loop_count);
                break;
            }
            LogInfo("  Second request in progress (loop {})...", loop_count);
        }

        // 关闭连接
        co_await client.close();
        LogInfo("✓ Connection closed\n");

    } catch (const std::exception& e) {
        LogError("❌ Exception: {}", e.what());
    }
    co_return;
}

/**
 * @brief 测试：正常请求（无超时）
 * @details 验证超时功能不影响正常请求
 */
Coroutine testNormalRequestWithTimeout(IOScheduler* scheduler)
{
    LogInfo("=== Test: Normal Request With Timeout ===");

    try {
        // 创建socket并连接
        TcpSocket socket(IPType::IPV4);
        auto nonblock_result = socket.option().handleNonBlock();
        if (!nonblock_result) {
            LogError("Failed to set non-block");
            co_return;
        }

        Host host(IPType::IPV4, TEST_HOST, TEST_PORT);
        auto connectResult = co_await socket.connect(host);

        if (!connectResult) {
            LogError("❌ Connect failed: {}", connectResult.error().message());
            co_return;
        }
        LogInfo("✓ Connected to server");

        // 创建 HttpClient
        HttpClient client(std::move(socket));

        // 发送正常请求，设置足够长的超时
        LogInfo("Sending GET request with 5s timeout...");

        int loop_count = 0;
        while (true) {
            loop_count++;
            auto result = co_await client.get("/api/data").timeout(5000ms);

            if (!result) {
                LogError("❌ Request failed: {}", result.error().message());
                break;
            } else if (result.value().has_value()) {
                auto& response = result.value().value();
                LogInfo("✓ Request succeeded");
                LogInfo("  Status: {}", static_cast<int>(response.header().code()));
                LogInfo("  Body: {}", response.getBodyStr());
                LogInfo("  Total loops: {}", loop_count);
                break;
            }
            // std::nullopt，继续循环
            LogInfo("  Request in progress (loop {})...", loop_count);
        }

        // 关闭连接
        co_await client.close();
        LogInfo("✓ Connection closed\n");

    } catch (const std::exception& e) {
        LogError("❌ Exception: {}", e.what());
    }
    co_return;
}

/**
 * @brief 主函数
 */
int main()
{
    LogInfo("==================================");
    LogInfo("HttpClient Timeout & Disconnect Tests");
    LogInfo("==================================\n");
    LogInfo("Note: These tests require a test server running on {}:{}", TEST_HOST, TEST_PORT);
    LogInfo("The server should support the following endpoints:");
    LogInfo("  - /delay/N: Delay N seconds before responding");
    LogInfo("  - /disconnect: Close connection immediately");
    LogInfo("  - /partial: Send partial response and stop");
    LogInfo("  - /api/data: Normal response\n");

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
        scheduler->spawn(testNormalRequestWithTimeout(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        scheduler->spawn(testRequestTimeout(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        scheduler->spawn(testConnectTimeout(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        scheduler->spawn(testServerDisconnect(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        scheduler->spawn(testReceiveTimeout(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        scheduler->spawn(testTimeoutRetry(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(5));

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
