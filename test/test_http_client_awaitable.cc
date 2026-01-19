/**
 * @file test_http_client_awaitable.cc
 * @brief HttpClientAwaitable 功能测试
 */

#include <iostream>
#include "galay-http/kernel/http/HttpClient.h"
#include "galay-kernel/kernel/Runtime.h"
#include "galay-kernel/common/Log.h"

using namespace galay::http;
using namespace galay::kernel;
using namespace galay::async;

/**
 * @brief 测试 GET 请求
 */
Coroutine testGet(IOScheduler* scheduler)
{
    LogInfo("=== Test 1: GET Request ===");

    // 创建socket并连接
    TcpSocket socket(IPType::IPV4);
    auto nonblock_result = socket.option().handleNonBlock();
    if (!nonblock_result) {
        LogError("Failed to set non-block");
        co_return;
    }

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        LogError("Failed to connect: {}", connect_result.error().message());
        co_return;
    }

    LogInfo("Connected to 127.0.0.1:8080");

    // 创建HttpClient
    HttpClient client(std::move(socket));

    // 使用 HttpClientAwaitable API
    // 现在可以在循环中调用 client.get()，每次都会创建新的 awaitable
    int loop_count = 0;
    while (true) {
        loop_count++;
        LogInfo("Loop iteration: {}", loop_count);

        auto result = co_await client.get("/api/info");

        if (!result) {
            // 错误处理
            LogError("Request failed: {}", result.error().message());
            break;
        }

        if (result.value().has_value()) {
            // 完成，获取响应
            HttpResponse response = std::move(result.value().value());
            LogInfo("✓ GET request completed successfully!");
            LogInfo("  Status: {} {}",
                    static_cast<int>(response.header().code()),
                    httpStatusCodeToString(response.header().code()));
            LogInfo("  Body: {}", response.getBodyStr());
            LogInfo("  Total loops: {}", loop_count);
            break;
        }

        // std::nullopt，继续循环
        LogInfo("  Request in progress, continuing...");
    }

    co_await client.close();
    LogInfo("");
    co_return;
}

/**
 * @brief 测试 POST 请求
 */
Coroutine testPost(IOScheduler* scheduler)
{
    LogInfo("=== Test 2: POST Request ===");

    // 创建socket并连接
    TcpSocket socket(IPType::IPV4);
    auto nonblock_result = socket.option().handleNonBlock();
    if (!nonblock_result) {
        LogError("Failed to set non-block");
        co_return;
    }

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        LogError("Failed to connect: {}", connect_result.error().message());
        co_return;
    }

    LogInfo("Connected to 127.0.0.1:8080");

    // 创建HttpClient
    HttpClient client(std::move(socket));

    // 使用 HttpClientAwaitable API 发送 POST 请求
    std::string body = R"({"name":"test","value":123})";
    int loop_count = 0;

    while (true) {
        loop_count++;
        LogInfo("Loop iteration: {}", loop_count);

        auto result = co_await client.post("/api/data", body, "application/json");

        if (!result) {
            LogError("Request failed: {}", result.error().message());
            break;
        }

        if (result.value().has_value()) {
            HttpResponse response = std::move(result.value().value());
            LogInfo("✓ POST request completed successfully!");
            LogInfo("  Status: {} {}",
                    static_cast<int>(response.header().code()),
                    httpStatusCodeToString(response.header().code()));
            LogInfo("  Total loops: {}", loop_count);
            break;
        }

        LogInfo("  Request in progress, continuing...");
    }

    co_await client.close();
    LogInfo("");
    co_return;
}

/**
 * @brief 测试多个连续请求
 */
Coroutine testMultipleRequests(IOScheduler* scheduler)
{
    LogInfo("=== Test 3: Multiple Requests ===");

    // 创建socket并连接
    TcpSocket socket(IPType::IPV4);
    auto nonblock_result = socket.option().handleNonBlock();
    if (!nonblock_result) {
        LogError("Failed to set non-block");
        co_return;
    }

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        LogError("Failed to connect: {}", connect_result.error().message());
        co_return;
    }

    LogInfo("Connected to 127.0.0.1:8080");

    // 创建HttpClient
    HttpClient client(std::move(socket));

    // 发送多个请求
    std::vector<std::string> uris = {"/", "/hello", "/test"};

    for (const auto& uri : uris) {
        LogInfo("Requesting: {}", uri);

        while (true) {
            auto result = co_await client.get(uri);

            if (!result) {
                LogError("Request failed: {}", result.error().message());
                break;
            }

            if (result.value().has_value()) {
                HttpResponse response = std::move(result.value().value());
                LogInfo("✓ Request to {} completed", uri);
                LogInfo("  Status: {}", static_cast<int>(response.header().code()));
                LogInfo("  Body length: {} bytes", response.getBodyStr().size());
                break;
            }
        }
    }

    co_await client.close();
    LogInfo("");
    co_return;
}

int main()
{
    LogInfo("========================================");
    LogInfo("HttpClientAwaitable Functionality Test");
    LogInfo("========================================\n");

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
        scheduler->spawn(testGet(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        scheduler->spawn(testPost(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        scheduler->spawn(testMultipleRequests(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        runtime.stop();

        LogInfo("========================================");
        LogInfo("All Tests Completed");
        LogInfo("========================================");

    } catch (const std::exception& e) {
        LogError("Test error: {}", e.what());
        return 1;
    }

    return 0;
}
