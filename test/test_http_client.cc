/**
 * @file test_http_client.cc
 * @brief HTTP Client 测试
 */

#include <iostream>
#include "galay-http/kernel/http/HttpClient.h"
#include "galay-kernel/kernel/Runtime.h"
#include "galay-kernel/common/Log.h"

using namespace galay::http;
using namespace galay::kernel;
using namespace galay::async;

/**
 * @brief 测试GET请求
 */
Coroutine testGet(IOScheduler* scheduler)
{
    LogInfo("Testing GET request...");

    // 创建socket并连接
    TcpSocket socket(IPType::IPV4);
    auto nonblock_result = socket.option().handleNonBlock();
    if (!nonblock_result) {
        LogError("Failed to set non-block");
        co_return;
    }

    Host host(IPType::IPV4, "93.184.216.34", 80);  // httpbin.org 的 IP
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        LogError("Failed to connect: {}", connect_result.error().message());
        co_return;
    }

    LogInfo("Connected to httpbin.org:80");

    // 创建HttpClient
    HttpClient client(std::move(socket));

    // 构造GET请求
    HttpRequest request;
    HttpRequestHeader header;
    header.method() = HttpMethod::GET;
    header.uri() = "/get";
    header.version() = HttpVersion::HttpVersion_1_1;
    header.headerPairs().addHeaderPair("Host", "httpbin.org");
    header.headerPairs().addHeaderPair("User-Agent", "galay-http-client/1.0");
    header.headerPairs().addHeaderPair("Connection", "close");
    request.setHeader(std::move(header));

    // 发送请求
    auto send_result = co_await client.sendRequest(request);
    if (!send_result) {
        LogError("Failed to send request: {}", send_result.error().message());
        co_await client.close();
        co_return;
    }

    LogInfo("Request sent: complete");

    // 接收响应
    HttpResponse response;
    bool complete = false;
    while (!complete) {
        auto recv_result = co_await client.getResponse(response);
        if (!recv_result) {
            LogError("Failed to receive response: {}", recv_result.error().message());
            co_await client.close();
            co_return;
        }
        complete = recv_result.value();
    }

    LogInfo("GET request successful:");
    LogInfo("  Status: {} {}",
            static_cast<int>(response.header().code()),
            response.header().reason());
    LogInfo("  Body length: {} bytes", response.getBodyStr().size());
    LogInfo("  Body preview: {}",
            response.getBodyStr().substr(0, std::min<size_t>(100, response.getBodyStr().size())));

    co_await client.close();
    co_return;
}

/**
 * @brief 测试POST请求
 */
Coroutine testPost(IOScheduler* scheduler)
{
    LogInfo("Testing POST request...");

    // 创建socket并连接
    TcpSocket socket(IPType::IPV4);
    auto nonblock_result = socket.option().handleNonBlock();
    if (!nonblock_result) {
        LogError("Failed to set non-block");
        co_return;
    }

    Host host(IPType::IPV4, "93.184.216.34", 80);  // httpbin.org 的 IP
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        LogError("Failed to connect: {}", connect_result.error().message());
        co_return;
    }

    LogInfo("Connected to httpbin.org:80");

    // 创建HttpClient
    HttpClient client(std::move(socket));

    // 构造POST请求
    HttpRequest request;
    HttpRequestHeader header;
    header.method() = HttpMethod::POST;
    header.uri() = "/post";
    header.version() = HttpVersion::HttpVersion_1_1;
    header.headerPairs().addHeaderPair("Host", "httpbin.org");
    header.headerPairs().addHeaderPair("User-Agent", "galay-http-client/1.0");
    header.headerPairs().addHeaderPair("Connection", "close");
    header.headerPairs().addHeaderPair("Content-Type", "application/x-www-form-urlencoded");
    request.setHeader(std::move(header));

    std::string body = "name=test&value=123";
    request.setBodyStr(std::move(body));

    // 发送请求
    auto send_result = co_await client.sendRequest(request);
    if (!send_result) {
        LogError("Failed to send request: {}", send_result.error().message());
        co_await client.close();
        co_return;
    }

    LogInfo("Request sent: complete");

    // 接收响应
    HttpResponse response;
    bool complete = false;
    while (!complete) {
        auto recv_result = co_await client.getResponse(response);
        if (!recv_result) {
            LogError("Failed to receive response: {}", recv_result.error().message());
            co_await client.close();
            co_return;
        }
        complete = recv_result.value();
    }

    LogInfo("POST request successful:");
    LogInfo("  Status: {} {}",
            static_cast<int>(response.header().code()),
            response.header().reason());
    LogInfo("  Body length: {} bytes", response.getBodyStr().size());

    co_await client.close();
    co_return;
}

/**
 * @brief 测试Chunked请求
 */
Coroutine testChunked(IOScheduler* scheduler)
{
    LogInfo("Testing Chunked POST request...");

    // 创建socket并连接
    TcpSocket socket(IPType::IPV4);
    auto nonblock_result = socket.option().handleNonBlock();
    if (!nonblock_result) {
        LogError("Failed to set non-block");
        co_return;
    }

    Host host(IPType::IPV4, "93.184.216.34", 80);  // httpbin.org 的 IP
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        LogError("Failed to connect: {}", connect_result.error().message());
        co_return;
    }

    LogInfo("Connected to httpbin.org:80");

    // 创建HttpClient
    HttpClient client(std::move(socket));

    // 构造POST请求（使用chunked编码）
    HttpRequest request;
    HttpRequestHeader header;
    header.method() = HttpMethod::POST;
    header.uri() = "/post";
    header.version() = HttpVersion::HttpVersion_1_1;
    header.headerPairs().addHeaderPair("Host", "httpbin.org");
    header.headerPairs().addHeaderPair("User-Agent", "galay-http-client/1.0");
    header.headerPairs().addHeaderPair("Connection", "close");
    header.headerPairs().addHeaderPair("Transfer-Encoding", "chunked");
    request.setHeader(std::move(header));

    // 发送请求头
    auto send_result = co_await client.sendRequest(request);
    if (!send_result) {
        LogError("Failed to send request header: {}", send_result.error().message());
        co_await client.close();
        co_return;
    }

    LogInfo("Request header sent: {} bytes", send_result.value());

    // 发送chunk数据
    std::string chunk1 = "Hello ";
    auto chunk1_result = co_await client.sendChunk(chunk1, false);
    if (!chunk1_result) {
        LogError("Failed to send chunk1: {}", chunk1_result.error().message());
        co_await client.close();
        co_return;
    }

    std::string chunk2 = "World!";
    auto chunk2_result = co_await client.sendChunk(chunk2, true);
    if (!chunk2_result) {
        LogError("Failed to send chunk2: {}", chunk2_result.error().message());
        co_await client.close();
        co_return;
    }

    LogInfo("Chunks sent");

    // 接收响应
    HttpResponse response;
    bool complete = false;
    while (!complete) {
        auto recv_result = co_await client.getResponse(response);
        if (!recv_result) {
            LogError("Failed to receive response: {}", recv_result.error().message());
            co_await client.close();
            co_return;
        }
        complete = recv_result.value();
    }

    LogInfo("Chunked POST request successful:");
    LogInfo("  Status: {} {}",
            static_cast<int>(response.header().code()),
            response.header().reason());
    LogInfo("  Body length: {} bytes", response.getBodyStr().size());

    co_await client.close();
    co_return;
}

int main()
{
    LogInfo("========================================");
    LogInfo("HTTP Client Test");
    LogInfo("========================================\n");

    try {
        // 创建 Runtime
        Runtime runtime;
        runtime.start();

        LogInfo("Runtime started with {} IO schedulers\n", runtime.getIOSchedulerCount());

        // 获取 IO 调度器
        auto* scheduler = runtime.getNextIOScheduler();
        if (!scheduler) {
            LogError("No IO scheduler available");
            return 1;
        }

        // 运行测试
        scheduler->spawn(testGet(scheduler));
        scheduler->spawn(testPost(scheduler));
        scheduler->spawn(testChunked(scheduler));

        // 等待一段时间让请求完成
        std::this_thread::sleep_for(std::chrono::seconds(15));

        // 停止 Runtime
        runtime.stop();

        LogInfo("\n========================================");
        LogInfo("HTTP Client Test Completed");
        LogInfo("========================================");

    } catch (const std::exception& e) {
        LogError("Test error: {}", e.what());
        return 1;
    }

    return 0;
}
