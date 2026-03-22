/**
 * @file test_http_client_timeout.cc
 * @brief HttpClient 超时和断连测试
 * @details 测试 HttpClientAwaitable 的超时功能和连接断开处理
 */

#include "galay-http/kernel/http/HttpClient.h"
#include "galay-kernel/kernel/Runtime.h"
#include "galay-http/kernel/http/HttpLog.h"
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
Task<void> testRequestTimeout(IOScheduler* scheduler)
{
    HTTP_LOG_INFO("=== Test: Request Timeout ===");

    try {
        // 创建socket并连接
        TcpSocket socket(IPType::IPV4);
        auto nonblock_result = socket.option().handleNonBlock();
        if (!nonblock_result) {
            HTTP_LOG_ERROR("Failed to set non-block");
            co_return;
        }

        Host host(IPType::IPV4, TEST_HOST, TEST_PORT);
        auto connectResult = co_await socket.connect(host);

        if (!connectResult) {
            HTTP_LOG_ERROR("❌ Connect failed: {}", connectResult.error().message());
            co_return;
        }
        HTTP_LOG_INFO("✓ Connected to server");

        // 创建 HttpClient
        HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());

        // 获取Session
        auto session = client.getSession();

        // 发送请求并设置 1 秒超时（假设服务器会延迟 5 秒响应）
        HTTP_LOG_INFO("Sending GET request with 1s timeout...");

        int loop_count = 0;
        while (true) {
            loop_count++;
            auto result = co_await session.get("/delay/5").timeout(1000ms);

            if (!result) {
                // 期望超时错误
                if (result.error().code() == kRequestTimeOut || result.error().code() == kRecvTimeOut) {
                    HTTP_LOG_INFO("✓ Request timed out as expected: {}", result.error().message());
                } else {
                    HTTP_LOG_ERROR("❌ Unexpected error: {}", result.error().message());
                }
                break;
            } else if (result.value().has_value()) {
                HTTP_LOG_ERROR("❌ Request should have timed out but succeeded");
                break;
            }
            // std::nullopt，继续循环
            HTTP_LOG_INFO("  Request in progress (loop {})...", loop_count);
        }

        // 关闭连接
        co_await client.close();
        HTTP_LOG_INFO("✓ Connection closed\n");

    } catch (const std::exception& e) {
        HTTP_LOG_ERROR("❌ Exception: {}", e.what());
    }
    co_return;
}

/**
 * @brief 测试：连接超时
 * @details 连接到不存在的服务器，测试连接超时
 */
Task<void> testConnectTimeout(IOScheduler* scheduler)
{
    HTTP_LOG_INFO("=== Test: Connect Timeout ===");

    try {
        // 尝试连接到不存在的服务器（使用不可路由的 IP）
        TcpSocket socket(IPType::IPV4);
        auto nonblock_result = socket.option().handleNonBlock();
        if (!nonblock_result) {
            HTTP_LOG_ERROR("Failed to set non-block");
            co_return;
        }

        HTTP_LOG_INFO("Attempting to connect to unreachable host with 2s timeout...");
        Host host(IPType::IPV4, "192.0.2.1", 9999);
        auto connectResult = co_await socket.connect(host).timeout(2000ms);

        if (!connectResult) {
            if (connectResult.error().code() == kTimeout) {
                HTTP_LOG_INFO("✓ Connect timed out as expected: {}", connectResult.error().message());
            } else {
                HTTP_LOG_INFO("⚠ Connect failed with error: {}", connectResult.error().message());
            }
        } else {
            HTTP_LOG_ERROR("❌ Connect should have timed out but succeeded");
        }

    } catch (const std::exception& e) {
        HTTP_LOG_ERROR("❌ Exception: {}", e.what());
    }

    HTTP_LOG_INFO("");
    co_return;
}

/**
 * @brief 测试：服务器主动断开连接
 * @details 服务器在发送部分数据后断开连接
 */
Task<void> testServerDisconnect(IOScheduler* scheduler)
{
    HTTP_LOG_INFO("=== Test: Server Disconnect ===");

    try {
        // 创建socket并连接
        TcpSocket socket(IPType::IPV4);
        auto nonblock_result = socket.option().handleNonBlock();
        if (!nonblock_result) {
            HTTP_LOG_ERROR("Failed to set non-block");
            co_return;
        }

        Host host(IPType::IPV4, TEST_HOST, TEST_PORT);
        auto connectResult = co_await socket.connect(host);

        if (!connectResult) {
            HTTP_LOG_ERROR("❌ Connect failed: {}", connectResult.error().message());
            co_return;
        }
        HTTP_LOG_INFO("✓ Connected to server");

        // 创建 HttpClient
        HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());

        // 获取Session
        auto session = client.getSession();

        // 请求一个会导致服务器断开连接的端点
        HTTP_LOG_INFO("Sending GET request to /disconnect endpoint...");

        int loop_count = 0;
        while (true) {
            loop_count++;
            auto result = co_await session.get("/disconnect");

            if (!result) {
                // 期望连接错误
                HTTP_LOG_INFO("✓ Detected server disconnect: {}", result.error().message());
                break;
            } else if (result.value().has_value()) {
                HTTP_LOG_ERROR("❌ Request should have failed but succeeded");
                break;
            }
            // std::nullopt，继续循环
            HTTP_LOG_INFO("  Request in progress (loop {})...", loop_count);
        }

        // 尝试关闭连接（可能已经断开）
        auto closeResult = co_await client.close();
        if (closeResult) {
            HTTP_LOG_INFO("✓ Connection closed");
        } else {
            HTTP_LOG_INFO("⚠ Close failed (connection may already be closed): {}",
                    closeResult.error().message());
        }

    } catch (const std::exception& e) {
        HTTP_LOG_ERROR("❌ Exception: {}", e.what());
    }

    HTTP_LOG_INFO("");
    co_return;
}

/**
 * @brief 测试：接收超时
 * @details 服务器发送部分数据后停止，测试接收超时
 */
Task<void> testReceiveTimeout(IOScheduler* scheduler)
{
    HTTP_LOG_INFO("=== Test: Receive Timeout ===");

    try {
        // 创建socket并连接
        TcpSocket socket(IPType::IPV4);
        auto nonblock_result = socket.option().handleNonBlock();
        if (!nonblock_result) {
            HTTP_LOG_ERROR("Failed to set non-block");
            co_return;
        }

        Host host(IPType::IPV4, TEST_HOST, TEST_PORT);
        auto connectResult = co_await socket.connect(host);

        if (!connectResult) {
            HTTP_LOG_ERROR("❌ Connect failed: {}", connectResult.error().message());
            co_return;
        }
        HTTP_LOG_INFO("✓ Connected to server");

        // 创建 HttpClient
        HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());

        // 请求一个会发送部分数据然后停止的端点
        HTTP_LOG_INFO("Sending GET request to /partial endpoint with 2s timeout...");
        auto session = client.getSession();

        int loop_count = 0;
        while (true) {
            loop_count++;
            auto result = co_await session.get("/partial").timeout(2000ms);

            if (!result) {
                if (result.error().code() == kRequestTimeOut || result.error().code() == kRecvTimeOut) {
                    HTTP_LOG_INFO("✓ Receive timed out as expected: {}", result.error().message());
                } else {
                    HTTP_LOG_INFO("⚠ Request failed with error: {}", result.error().message());
                }
                break;
            } else if (result.value().has_value()) {
                HTTP_LOG_ERROR("❌ Request should have timed out but succeeded");
                break;
            }
            // std::nullopt，继续循环
            HTTP_LOG_INFO("  Request in progress (loop {})...", loop_count);
        }

        // 关闭连接
        co_await client.close();
        HTTP_LOG_INFO("✓ Connection closed\n");

    } catch (const std::exception& e) {
        HTTP_LOG_ERROR("❌ Exception: {}", e.what());
    }
    co_return;
}

/**
 * @brief 测试：多次超时重试
 * @details 测试超时后重新发起请求
 */
Task<void> testTimeoutRetry(IOScheduler* scheduler)
{
    HTTP_LOG_INFO("=== Test: Timeout Retry ===");

    try {
        // 创建socket并连接
        TcpSocket socket(IPType::IPV4);
        auto nonblock_result = socket.option().handleNonBlock();
        if (!nonblock_result) {
            HTTP_LOG_ERROR("Failed to set non-block");
            co_return;
        }

        Host host(IPType::IPV4, TEST_HOST, TEST_PORT);
        auto connectResult = co_await socket.connect(host);

        if (!connectResult) {
            HTTP_LOG_ERROR("❌ Connect failed: {}", connectResult.error().message());
            co_return;
        }
        HTTP_LOG_INFO("✓ Connected to server");

        // 创建 HttpClient
        HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());

        auto session = client.getSession();

        // 第一次请求：超时
        HTTP_LOG_INFO("First request with 1s timeout...");
        int loop_count = 0;
        while (true) {
            loop_count++;
            auto result1 = co_await session.get("/delay/5").timeout(1000ms);

            if (!result1) {
                if (result1.error().code() == kRequestTimeOut || result1.error().code() == kRecvTimeOut) {
                    HTTP_LOG_INFO("✓ First request timed out as expected");
                } else {
                    HTTP_LOG_INFO("⚠ First request failed: {}", result1.error().message());
                }
                break;
            } else if (result1.value().has_value()) {
                HTTP_LOG_INFO("⚠ First request did not timeout as expected");
                break;
            }
            HTTP_LOG_INFO("  First request in progress (loop {})...", loop_count);
        }
        // 第二次请求：正常完成
        HTTP_LOG_INFO("Second request with sufficient timeout...");
        loop_count = 0;
        while (true) {
            loop_count++;
            auto result2 = co_await session.get("/api/data").timeout(5000ms);

            if (!result2) {
                HTTP_LOG_INFO("⚠ Second request failed: {}", result2.error().message());
                break;
            } else if (result2.value().has_value()) {
                auto& response = result2.value().value();
                HTTP_LOG_INFO("✓ Second request succeeded");
                HTTP_LOG_INFO("  Status: {}", static_cast<int>(response.header().code()));
                HTTP_LOG_INFO("  Total loops: {}", loop_count);
                break;
            }
            HTTP_LOG_INFO("  Second request in progress (loop {})...", loop_count);
        }

        // 关闭连接
        co_await client.close();
        HTTP_LOG_INFO("✓ Connection closed\n");

    } catch (const std::exception& e) {
        HTTP_LOG_ERROR("❌ Exception: {}", e.what());
    }
    co_return;
}

/**
 * @brief 测试：正常请求（无超时）
 * @details 验证超时功能不影响正常请求
 */
Task<void> testNormalRequestWithTimeout(IOScheduler* scheduler)
{
    HTTP_LOG_INFO("=== Test: Normal Request With Timeout ===");

    try {
        // 创建socket并连接
        TcpSocket socket(IPType::IPV4);
        auto nonblock_result = socket.option().handleNonBlock();
        if (!nonblock_result) {
            HTTP_LOG_ERROR("Failed to set non-block");
            co_return;
        }

        Host host(IPType::IPV4, TEST_HOST, TEST_PORT);
        auto connectResult = co_await socket.connect(host);

        if (!connectResult) {
            HTTP_LOG_ERROR("❌ Connect failed: {}", connectResult.error().message());
            co_return;
        }
        HTTP_LOG_INFO("✓ Connected to server");

        // 创建 HttpClient
        HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());

        // 发送正常请求，设置足够长的超时
        HTTP_LOG_INFO("Sending GET request with 5s timeout...");
        auto session = client.getSession();
        int loop_count = 0;
        while (true) {
            loop_count++;
            auto result = co_await session.get("/api/data").timeout(5000ms);

            if (!result) {
                HTTP_LOG_ERROR("❌ Request failed: {}", result.error().message());
                break;
            } else if (result.value().has_value()) {
                auto& response = result.value().value();
                HTTP_LOG_INFO("✓ Request succeeded");
                HTTP_LOG_INFO("  Status: {}", static_cast<int>(response.header().code()));
                HTTP_LOG_INFO("  Body: {}", response.getBodyStr());
                HTTP_LOG_INFO("  Total loops: {}", loop_count);
                break;
            }
            // std::nullopt，继续循环
            HTTP_LOG_INFO("  Request in progress (loop {})...", loop_count);
        }

        // 关闭连接
        co_await client.close();
        HTTP_LOG_INFO("✓ Connection closed\n");

    } catch (const std::exception& e) {
        HTTP_LOG_ERROR("❌ Exception: {}", e.what());
    }
    co_return;
}

/**
 * @brief 主函数
 */
int main()
{
    HTTP_LOG_INFO("==================================");
    HTTP_LOG_INFO("HttpClient Timeout & Disconnect Tests");
    HTTP_LOG_INFO("==================================\n");
    HTTP_LOG_INFO("Note: These tests require a test server running on {}:{}", TEST_HOST, TEST_PORT);
    HTTP_LOG_INFO("The server should support the following endpoints:");
    HTTP_LOG_INFO("  - /delay/N: Delay N seconds before responding");
    HTTP_LOG_INFO("  - /disconnect: Close connection immediately");
    HTTP_LOG_INFO("  - /partial: Send partial response and stop");
    HTTP_LOG_INFO("  - /api/data: Normal response\n");

    try {
        Runtime runtime;
        runtime.start();

        HTTP_LOG_INFO("Runtime started with {} IO schedulers\n", runtime.getIOSchedulerCount());

        auto* scheduler = runtime.getNextIOScheduler();
        if (!scheduler) {
            HTTP_LOG_ERROR("No IO scheduler available");
            return 1;
        }

        // 运行测试
        scheduleTask(scheduler, testNormalRequestWithTimeout(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        scheduleTask(scheduler, testRequestTimeout(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        scheduleTask(scheduler, testConnectTimeout(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        scheduleTask(scheduler, testServerDisconnect(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        scheduleTask(scheduler, testReceiveTimeout(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        scheduleTask(scheduler, testTimeoutRetry(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(5));

        runtime.stop();

        HTTP_LOG_INFO("==================================");
        HTTP_LOG_INFO("All Tests Completed");
        HTTP_LOG_INFO("==================================");

    } catch (const std::exception& e) {
        HTTP_LOG_ERROR("Fatal error: {}", e.what());
        return 1;
    }

    return 0;
}
