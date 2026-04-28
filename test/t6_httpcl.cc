/**
 * @file test_http_client_awaitable.cc
 * @brief HttpClientAwaitable 功能测试
 */

#include "galay-http/kernel/http/http_client.h"
#include "galay-kernel/kernel/runtime.h"
#include "galay-http/kernel/http/http_log.h"

using namespace galay::http;
using namespace galay::kernel;
using namespace galay::async;

/**
 * @brief 测试 GET 请求
 */
Task<void> testGet(IOScheduler* scheduler)
{
    HTTP_LOG_INFO("=== Test 1: GET Request ===");

    // 创建socket并连接
    TcpSocket socket(IPType::IPV4);
    auto nonblock_result = socket.option().handleNonBlock();
    if (!nonblock_result) {
        HTTP_LOG_ERROR("Failed to set non-block");
        co_return;
    }

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        HTTP_LOG_ERROR("Failed to connect: {}", connect_result.error().message());
        co_return;
    }

    HTTP_LOG_INFO("Connected to 127.0.0.1:8080");

    // 创建HttpClient
    HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());
    auto session = client.getSession();
    // 使用 HttpClientAwaitable API
    // 现在可以在循环中调用 client.get()，每次都会创建新的 awaitable
    int loop_count = 0;
    while (true) {
        loop_count++;
        HTTP_LOG_INFO("Loop iteration: {}", loop_count);

        auto result = co_await session.get("/api/info");

        if (!result) {
            // 错误处理
            HTTP_LOG_ERROR("Request failed: {}", result.error().message());
            break;
        }

        if (result.value().has_value()) {
            // 完成，获取响应
            HttpResponse response = std::move(result.value().value());
            HTTP_LOG_INFO("✓ GET request completed successfully!");
            HTTP_LOG_INFO("  Status: {} {}",
                    static_cast<int>(response.header().code()),
                    httpStatusCodeToString(response.header().code()));
            HTTP_LOG_INFO("  Body: {}", response.getBodyStr());
            HTTP_LOG_INFO("  Total loops: {}", loop_count);
            break;
        }

        // std::nullopt，继续循环
        HTTP_LOG_INFO("  Request in progress, continuing...");
    }

    co_await client.close();
    HTTP_LOG_INFO("");
    co_return;
}

/**
 * @brief 测试 POST 请求
 */
Task<void> testPost(IOScheduler* scheduler)
{
    HTTP_LOG_INFO("=== Test 2: POST Request ===");

    // 创建socket并连接
    TcpSocket socket(IPType::IPV4);
    auto nonblock_result = socket.option().handleNonBlock();
    if (!nonblock_result) {
        HTTP_LOG_ERROR("Failed to set non-block");
        co_return;
    }

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        HTTP_LOG_ERROR("Failed to connect: {}", connect_result.error().message());
        co_return;
    }

    HTTP_LOG_INFO("Connected to 127.0.0.1:8080");

    // 创建HttpClient
    HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());

    // 使用 HttpClientAwaitable API 发送 POST 请求
    std::string body = R"({"name":"test","value":123})";
    int loop_count = 0;
    auto session = client.getSession();
    while (true) {
        loop_count++;
        HTTP_LOG_INFO("Loop iteration: {}", loop_count);

        auto result = co_await session.post("/api/data", body, "application/json");

        if (!result) {
            HTTP_LOG_ERROR("Request failed: {}", result.error().message());
            break;
        }

        if (result.value().has_value()) {
            HttpResponse response = std::move(result.value().value());
            HTTP_LOG_INFO("✓ POST request completed successfully!");
            HTTP_LOG_INFO("  Status: {} {}",
                    static_cast<int>(response.header().code()),
                    httpStatusCodeToString(response.header().code()));
            HTTP_LOG_INFO("  Total loops: {}", loop_count);
            break;
        }

        HTTP_LOG_INFO("  Request in progress, continuing...");
    }

    co_await client.close();
    HTTP_LOG_INFO("");
    co_return;
}

/**
 * @brief 测试多个连续请求
 */
Task<void> testMultipleRequests(IOScheduler* scheduler)
{
    HTTP_LOG_INFO("=== Test 3: Multiple Requests ===");

    // 创建socket并连接
    TcpSocket socket(IPType::IPV4);
    auto nonblock_result = socket.option().handleNonBlock();
    if (!nonblock_result) {
        HTTP_LOG_ERROR("Failed to set non-block");
        co_return;
    }

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        HTTP_LOG_ERROR("Failed to connect: {}", connect_result.error().message());
        co_return;
    }

    HTTP_LOG_INFO("Connected to 127.0.0.1:8080");

    // 创建HttpClient
    HttpClient client(std::move(socket), HttpClientBuilder().buildConfig());

    // 发送多个请求
    std::vector<std::string> uris = {"/", "/hello", "/test"};
    auto session = client.getSession();
    bool session_alive = true;
    for (const auto& uri : uris) {
        if (!session_alive) {
            break;
        }
        HTTP_LOG_INFO("Requesting: {}", uri);

        while (true) {
            auto result = co_await session.get(uri);

            if (!result) {
                HTTP_LOG_ERROR("Request failed: {}", result.error().message());
                session_alive = false;
                break;
            }

            if (result.value().has_value()) {
                HttpResponse response = std::move(result.value().value());
                HTTP_LOG_INFO("✓ Request to {} completed", uri);
                HTTP_LOG_INFO("  Status: {}", static_cast<int>(response.header().code()));
                HTTP_LOG_INFO("  Body length: {} bytes", response.getBodyStr().size());
                break;
            }
        }
    }

    co_await client.close();
    HTTP_LOG_INFO("");
    co_return;
}

int main()
{
    HTTP_LOG_INFO("========================================");
    HTTP_LOG_INFO("HttpClientAwaitable Functionality Test");
    HTTP_LOG_INFO("========================================\n");

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
        scheduleTask(scheduler, testGet(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        scheduleTask(scheduler, testPost(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        scheduleTask(scheduler, testMultipleRequests(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        runtime.stop();

        HTTP_LOG_INFO("========================================");
        HTTP_LOG_INFO("All Tests Completed");
        HTTP_LOG_INFO("========================================");

    } catch (const std::exception& e) {
        HTTP_LOG_ERROR("Test error: {}", e.what());
        return 1;
    }

    return 0;
}
