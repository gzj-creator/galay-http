/**
 * @file test_http_client_awaitable_edge_cases.cc
 * @brief HttpClientAwaitable 边界测试
 */

#include <iostream>
#include "galay-http/kernel/http/HttpClient.h"
#include "galay-kernel/kernel/Runtime.h"
#include "galay-http/kernel/http/HttpLog.h"

using namespace galay::http;
using namespace galay::kernel;
using namespace galay::async;

/**
 * @brief 测试1: 连接失败
 */
Task<void> testConnectionFailure(IOScheduler* scheduler)
{
    HTTP_LOG_INFO("=== Test 1: Connection Failure ===");

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    // 连接到不存在的服务器
    Host host(IPType::IPV4, "127.0.0.1", 9999);
    auto connect_result = co_await socket.connect(host);

    if (!connect_result) {
        HTTP_LOG_INFO("✓ Connection failed as expected: {}", connect_result.error().message());
    } else {
        HTTP_LOG_ERROR("✗ Connection should have failed");
    }

    HTTP_LOG_INFO("");
    co_return;
}

/**
 * @brief 测试2: 服务器关闭连接
 */
Task<void> testServerCloseConnection(IOScheduler* scheduler)
{
    HTTP_LOG_INFO("=== Test 2: Server Close Connection ===");

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        HTTP_LOG_ERROR("Failed to connect: {}", connect_result.error().message());
        co_return;
    }

    HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());
    auto session = client.getSession();
    // 发送请求后立即关闭连接
    int loop_count = 0;
    while (true) {
        loop_count++;
        auto result = co_await session.get("/");

        if (!result) {
            HTTP_LOG_INFO("✓ Got error after {} loops: {}", loop_count, result.error().message());
            break;
        }

        if (result.value().has_value()) {
            HTTP_LOG_INFO("✓ Request completed after {} loops", loop_count);
            break;
        }

        if (loop_count > 100) {
            HTTP_LOG_ERROR("✗ Too many loops, something is wrong");
            break;
        }
    }

    co_await client.close();
    HTTP_LOG_INFO("");
    co_return;
}

/**
 * @brief 测试3: 多个连续请求
 */
Task<void> testMultipleRequests(IOScheduler* scheduler)
{
    HTTP_LOG_INFO("=== Test 3: Multiple Sequential Requests ===");

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        HTTP_LOG_ERROR("Failed to connect");
        co_return;
    }

    HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());
    auto session = client.getSession();
    // 发送3个连续请求
    for (int i = 0; i < 3; i++) {
        HTTP_LOG_INFO("Request #{}", i + 1);

        int loop_count = 0;
        while (true) {
            loop_count++;
            auto result = co_await session.get("/api/info");

            if (!result) {
                HTTP_LOG_ERROR("✗ Request #{} failed: {}", i + 1, result.error().message());
                co_await client.close();
                co_return;
            }

            if (result.value().has_value()) {
                HttpResponse response = std::move(result.value().value());
                HTTP_LOG_INFO("✓ Request #{} completed after {} loops, status: {}",
                       i + 1, loop_count, static_cast<int>(response.header().code()));
                break;
            }

            if (loop_count > 100) {
                HTTP_LOG_ERROR("✗ Request #{} too many loops", i + 1);
                co_await client.close();
                co_return;
            }
        }
    }

    co_await client.close();
    HTTP_LOG_INFO("");
    co_return;
}

/**
 * @brief 测试4: 大请求体
 */
Task<void> testLargeRequestBody(IOScheduler* scheduler)
{
    HTTP_LOG_INFO("=== Test 4: Large Request Body ===");

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        HTTP_LOG_ERROR("Failed to connect");
        co_return;
    }

    HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());
    auto session = client.getSession();

    // 创建一个大的请求体 (10KB)
    std::string large_body(10240, 'A');

    int loop_count = 0;
    while (true) {
        loop_count++;
        auto result = co_await session.post("/api/data", large_body, "text/plain");

        if (!result) {
            HTTP_LOG_INFO("Request failed (expected for large body): {}", result.error().message());
            break;
        }

        if (result.value().has_value()) {
            HttpResponse response = std::move(result.value().value());
            HTTP_LOG_INFO("✓ Large request completed after {} loops, status: {}",
                   loop_count, static_cast<int>(response.header().code()));
            break;
        }

        if (loop_count > 100) {
            HTTP_LOG_ERROR("✗ Too many loops");
            break;
        }
    }

    co_await client.close();
    HTTP_LOG_INFO("");
    co_return;
}

/**
 * @brief 测试5: 404 错误
 */
Task<void> test404NotFound(IOScheduler* scheduler)
{
    HTTP_LOG_INFO("=== Test 5: 404 Not Found ===");

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        HTTP_LOG_ERROR("Failed to connect");
        co_return;
    }

    HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());
    auto session = client.getSession();
    int loop_count = 0;
    while (true) {
        loop_count++;
        auto result = co_await session.get("/nonexistent");

        if (!result) {
            HTTP_LOG_ERROR("✗ Request failed: {}", result.error().message());
            break;
        }

        if (result.value().has_value()) {
            HttpResponse response = std::move(result.value().value());
            auto status_code = static_cast<int>(response.header().code());
            if (status_code == 404) {
                HTTP_LOG_INFO("✓ Got 404 as expected after {} loops", loop_count);
            } else {
                HTTP_LOG_ERROR("✗ Expected 404 but got {}", status_code);
            }
            break;
        }

        if (loop_count > 100) {
            HTTP_LOG_ERROR("✗ Too many loops");
            break;
        }
    }

    co_await client.close();
    HTTP_LOG_INFO("");
    co_return;
}

/**
 * @brief 测试6: 空响应体
 */
Task<void> testEmptyResponse(IOScheduler* scheduler)
{
    HTTP_LOG_INFO("=== Test 6: Empty Response Body ===");

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        HTTP_LOG_ERROR("Failed to connect");
        co_return;
    }

    HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());
    auto session = client.getSession();
    int loop_count = 0;
    while (true) {
        loop_count++;
        auto result = co_await session.del("/api/resource");

        if (!result) {
            HTTP_LOG_INFO("Request failed: {}", result.error().message());
            break;
        }

        if (result.value().has_value()) {
            HttpResponse response = std::move(result.value().value());
            HTTP_LOG_INFO("✓ DELETE request completed after {} loops, body size: {}",
                   loop_count, response.getBodyStr().size());
            break;
        }

        if (loop_count > 100) {
            HTTP_LOG_ERROR("✗ Too many loops");
            break;
        }
    }

    co_await client.close();
    HTTP_LOG_INFO("");
    co_return;
}

int main()
{
    HTTP_LOG_INFO("========================================");
    HTTP_LOG_INFO("HttpClientAwaitable Edge Cases Test");
    HTTP_LOG_INFO("========================================\n");

    try {
        Runtime runtime;
        runtime.start();

        auto* scheduler = runtime.getNextIOScheduler();
        if (!scheduler) {
            HTTP_LOG_ERROR("No IO scheduler available");
            return 1;
        }

        // 运行边界测试
        scheduleTask(scheduler, testConnectionFailure(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        scheduleTask(scheduler, testServerCloseConnection(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        scheduleTask(scheduler, testMultipleRequests(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        scheduleTask(scheduler, testLargeRequestBody(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        scheduleTask(scheduler, test404NotFound(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        scheduleTask(scheduler, testEmptyResponse(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        runtime.stop();

        HTTP_LOG_INFO("========================================");
        HTTP_LOG_INFO("All Edge Cases Tests Completed");
        HTTP_LOG_INFO("========================================");

    } catch (const std::exception& e) {
        HTTP_LOG_ERROR("Test error: {}", e.what());
        return 1;
    }

    return 0;
}
