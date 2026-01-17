/**
 * @file test_http_client_awaitable_edge_cases.cc
 * @brief HttpClientAwaitable 边界测试
 */

#include <iostream>
#include "galay-http/kernel/http/HttpClient.h"
#include "galay-kernel/kernel/Runtime.h"
#include "galay-kernel/common/Log.h"

using namespace galay::http;
using namespace galay::kernel;
using namespace galay::async;

/**
 * @brief 测试1: 连接失败
 */
Coroutine testConnectionFailure(IOScheduler* scheduler)
{
    LogInfo("=== Test 1: Connection Failure ===");

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    // 连接到不存在的服务器
    Host host(IPType::IPV4, "127.0.0.1", 9999);
    auto connect_result = co_await socket.connect(host);

    if (!connect_result) {
        LogInfo("✓ Connection failed as expected: {}", connect_result.error().message());
    } else {
        LogError("✗ Connection should have failed");
    }

    LogInfo("");
    co_return;
}

/**
 * @brief 测试2: 服务器关闭连接
 */
Coroutine testServerCloseConnection(IOScheduler* scheduler)
{
    LogInfo("=== Test 2: Server Close Connection ===");

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        LogError("Failed to connect: {}", connect_result.error().message());
        co_return;
    }

    HttpClient client(std::move(socket));

    // 发送请求后立即关闭连接
    int loop_count = 0;
    while (true) {
        loop_count++;
        auto result = co_await client.get("/");

        if (!result) {
            LogInfo("✓ Got error after {} loops: {}", loop_count, result.error().message());
            break;
        }

        if (result.value().has_value()) {
            LogInfo("✓ Request completed after {} loops", loop_count);
            break;
        }

        if (loop_count > 100) {
            LogError("✗ Too many loops, something is wrong");
            break;
        }
    }

    co_await client.close();
    LogInfo("");
    co_return;
}

/**
 * @brief 测试3: 多个连续请求
 */
Coroutine testMultipleRequests(IOScheduler* scheduler)
{
    LogInfo("=== Test 3: Multiple Sequential Requests ===");

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        LogError("Failed to connect");
        co_return;
    }

    HttpClient client(std::move(socket));

    // 发送3个连续请求
    for (int i = 0; i < 3; i++) {
        LogInfo("Request #{}", i + 1);

        int loop_count = 0;
        while (true) {
            loop_count++;
            auto result = co_await client.get("/api/info");

            if (!result) {
                LogError("✗ Request #{} failed: {}", i + 1, result.error().message());
                co_await client.close();
                co_return;
            }

            if (result.value().has_value()) {
                HttpResponse response = std::move(result.value().value());
                LogInfo("✓ Request #{} completed after {} loops, status: {}",
                       i + 1, loop_count, static_cast<int>(response.header().code()));
                break;
            }

            if (loop_count > 100) {
                LogError("✗ Request #{} too many loops", i + 1);
                co_await client.close();
                co_return;
            }
        }
    }

    co_await client.close();
    LogInfo("");
    co_return;
}

/**
 * @brief 测试4: 大请求体
 */
Coroutine testLargeRequestBody(IOScheduler* scheduler)
{
    LogInfo("=== Test 4: Large Request Body ===");

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        LogError("Failed to connect");
        co_return;
    }

    HttpClient client(std::move(socket));

    // 创建一个大的请求体 (10KB)
    std::string large_body(10240, 'A');

    int loop_count = 0;
    while (true) {
        loop_count++;
        auto result = co_await client.post("/api/data", large_body, "text/plain");

        if (!result) {
            LogInfo("Request failed (expected for large body): {}", result.error().message());
            break;
        }

        if (result.value().has_value()) {
            HttpResponse response = std::move(result.value().value());
            LogInfo("✓ Large request completed after {} loops, status: {}",
                   loop_count, static_cast<int>(response.header().code()));
            break;
        }

        if (loop_count > 100) {
            LogError("✗ Too many loops");
            break;
        }
    }

    co_await client.close();
    LogInfo("");
    co_return;
}

/**
 * @brief 测试5: 404 错误
 */
Coroutine test404NotFound(IOScheduler* scheduler)
{
    LogInfo("=== Test 5: 404 Not Found ===");

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        LogError("Failed to connect");
        co_return;
    }

    HttpClient client(std::move(socket));

    int loop_count = 0;
    while (true) {
        loop_count++;
        auto result = co_await client.get("/nonexistent");

        if (!result) {
            LogError("✗ Request failed: {}", result.error().message());
            break;
        }

        if (result.value().has_value()) {
            HttpResponse response = std::move(result.value().value());
            auto status_code = static_cast<int>(response.header().code());
            if (status_code == 404) {
                LogInfo("✓ Got 404 as expected after {} loops", loop_count);
            } else {
                LogError("✗ Expected 404 but got {}", status_code);
            }
            break;
        }

        if (loop_count > 100) {
            LogError("✗ Too many loops");
            break;
        }
    }

    co_await client.close();
    LogInfo("");
    co_return;
}

/**
 * @brief 测试6: 空响应体
 */
Coroutine testEmptyResponse(IOScheduler* scheduler)
{
    LogInfo("=== Test 6: Empty Response Body ===");

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        LogError("Failed to connect");
        co_return;
    }

    HttpClient client(std::move(socket));

    int loop_count = 0;
    while (true) {
        loop_count++;
        auto result = co_await client.del("/api/resource");

        if (!result) {
            LogInfo("Request failed: {}", result.error().message());
            break;
        }

        if (result.value().has_value()) {
            HttpResponse response = std::move(result.value().value());
            LogInfo("✓ DELETE request completed after {} loops, body size: {}",
                   loop_count, response.getBodyStr().size());
            break;
        }

        if (loop_count > 100) {
            LogError("✗ Too many loops");
            break;
        }
    }

    co_await client.close();
    LogInfo("");
    co_return;
}

int main()
{
    LogInfo("========================================");
    LogInfo("HttpClientAwaitable Edge Cases Test");
    LogInfo("========================================\n");

    try {
        Runtime runtime;
        runtime.start();

        auto* scheduler = runtime.getNextIOScheduler();
        if (!scheduler) {
            LogError("No IO scheduler available");
            return 1;
        }

        // 运行边界测试
        scheduler->spawn(testConnectionFailure(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        scheduler->spawn(testServerCloseConnection(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        scheduler->spawn(testMultipleRequests(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        scheduler->spawn(testLargeRequestBody(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        scheduler->spawn(test404NotFound(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        scheduler->spawn(testEmptyResponse(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        runtime.stop();

        LogInfo("========================================");
        LogInfo("All Edge Cases Tests Completed");
        LogInfo("========================================");

    } catch (const std::exception& e) {
        LogError("Test error: {}", e.what());
        return 1;
    }

    return 0;
}
