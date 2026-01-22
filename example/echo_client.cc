/**
 * @file echo_client.cc
 * @brief Echo 客户端最小实践示例
 * @details 演示如何使用 HttpClient 发送请求到 Echo 服务器
 */

#include "galay-http/kernel/http/HttpClient.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/utils/Http1_1RequestBuilder.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/common/Log.h"
#include "galay-kernel/kernel/Runtime.h"
#include <iostream>
#include <string>

using namespace galay::http;
using namespace galay::kernel;
using namespace galay::async;

// 发送 Echo 请求的协程
Coroutine sendEchoRequest(Runtime& runtime, const std::string& host, int port, const std::string& message) {
    LogInfo("Connecting to {}:{}...", host, port);

    // 创建 TCP Socket
    TcpSocket socket(IPType::IPV4);

    // 设置非阻塞
    auto nonblock_result = socket.option().handleNonBlock();
    if (!nonblock_result) {
        LogError("Failed to set non-blocking: {}", nonblock_result.error().message());
        co_return;
    }

    // 连接到服务器
    Host server_host(IPType::IPV4, host, port);
    auto connect_result = co_await socket.connect(server_host);
    if (!connect_result) {
        LogError("Failed to connect to server: {}", connect_result.error().message());
        co_return;
    }

    LogInfo("Connected to server successfully");

    // 创建 HttpClient
    HttpClient client(std::move(socket));

    // 使用 Builder 构造 POST 请求 - 简洁优雅！
    auto request = Http1_1RequestBuilder::post("/echo")
        .host(host + ":" + std::to_string(port))
        .contentType("text/plain")
        .connection("close")
        .body(message)
        .build();

    LogInfo("Sending request: POST /echo");
    LogInfo("Request body: {}", message);

    // 发送请求
    auto writer = client.getWriter();
    auto send_result = co_await writer.sendRequest(request);
    if (!send_result) {
        LogError("Failed to send request: {}", send_result.error().message());
        co_return;
    }

    LogInfo("Request sent, waiting for response...");

    // 接收响应
    auto reader = client.getReader();
    HttpResponse response;
    auto recv_result = co_await reader.getResponse(response);
    if (!recv_result) {
        LogError("Failed to receive response: {}", recv_result.error().message());
        co_return;
    }

    // 打印响应
    LogInfo("Response received:");
    LogInfo("  Status: {} {}",
            static_cast<int>(response.header().code()),
            httpStatusCodeToString(response.header().code()));
    LogInfo("  Body: {}", response.getBodyStr());

    // 关闭连接
    co_await client.close();
    LogInfo("Connection closed");

    co_return;
}

int main(int argc, char* argv[]) {
    // 解析命令行参数
    std::string host = "127.0.0.1";
    int port = 8080;
    std::string message = "Hello, Echo Server!";

    if (argc > 1) {
        host = argv[1];
    }
    if (argc > 2) {
        port = std::atoi(argv[2]);
    }
    if (argc > 3) {
        message = argv[3];
    }

    std::cout << "========================================\n";
    std::cout << "Echo Client Example\n";
    std::cout << "========================================\n";
    std::cout << "Server: " << host << ":" << port << "\n";
    std::cout << "Message: " << message << "\n";
    std::cout << "========================================\n\n";

    try {
        // 创建 Runtime
        Runtime runtime(LoadBalanceStrategy::ROUND_ROBIN, 1, 1);
        runtime.start();

        LogInfo("Runtime started");

        // 获取调度器并启动请求协程
        auto* scheduler = runtime.getNextIOScheduler();
        if (!scheduler) {
            LogError("No IO scheduler available");
            return 1;
        }

        scheduler->spawn(sendEchoRequest(runtime, host, port, message));

        // 等待一段时间让请求完成
        std::this_thread::sleep_for(std::chrono::seconds(3));

        // 停止 Runtime
        runtime.stop();
        LogInfo("Runtime stopped");

    } catch (const std::exception& e) {
        LogError("Client error: {}", e.what());
        return 1;
    }

    return 0;
}
