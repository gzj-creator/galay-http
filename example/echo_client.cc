/**
 * @file echo_client.cc
 * @brief Echo 客户端最小实践示例
 * @details 演示如何使用 HttpClient 发送请求到 Echo 服务器
 */

#include "galay-http/kernel/http/HttpClient.h"
#include "galay-kernel/kernel/Runtime.h"
#include <iostream>
#include <string>

using namespace galay::http;
using namespace galay::kernel;

// 发送 Echo 请求的协程
Coroutine sendEchoRequest(Runtime& runtime, const std::string& url, const std::string& message) {
    std::cout << "Connecting to " << url << "...\n";

    // 创建 HttpClient 并连接
    HttpClient client;
    auto connect_result = co_await client.connect(url);
    if (!connect_result) {
        std::cerr << "Failed to connect to server: " << connect_result.error().message() << "\n";
        co_return;
    }

    std::cout << "Connected to server successfully\n";
    std::cout << "Sending request: POST " << client.url().path << "\n";
    std::cout << "Request body: " << message << "\n";

    // 使用 HttpClient 的 post 方法发送请求并接收响应
    while(true) {
        auto result = co_await client.post(client.url().path, message, "text/plain", {
            {"Host", client.url().host + ":" + std::to_string(client.url().port)},
            {"Connection", "close"}
        });
    
        if (!result) {
            std::cerr << "Failed to send/receive: " << result.error().message() << "\n";
            co_return;
        }
    
        if (!result.value()) {
            std::cerr << "Request incomplete\n";
            continue;
        }
    
        auto response = result.value().value();
    
        // 打印响应
        std::cout << "Response received:\n";
        std::cout << "  Status: " << static_cast<int>(response.header().code())
                  << " " << httpStatusCodeToString(response.header().code()) << "\n";
        std::cout << "  Body: " << response.getBodyStr() << "\n";
        break;
    
    }
    // 关闭连接
    co_await client.close();
    std::cout << "Connection closed\n";

    co_return;
}

int main(int argc, char* argv[]) {
    // 解析命令行参数
    std::string url = "http://127.0.0.1:8080/echo";
    std::string message = "Hello, Echo Server!";

    if (argc > 1) {
        url = argv[1];
    }
    if (argc > 2) {
        message = argv[2];
    }

    std::cout << "========================================\n";
    std::cout << "Echo Client Example\n";
    std::cout << "========================================\n";
    std::cout << "URL: " << url << "\n";
    std::cout << "Message: " << message << "\n";
    std::cout << "========================================\n\n";

    try {
        // 创建 Runtime
        Runtime runtime(LoadBalanceStrategy::ROUND_ROBIN, 1, 1);
        runtime.start();

        std::cout << "Runtime started\n";

        // 获取调度器并启动请求协程
        auto* scheduler = runtime.getNextIOScheduler();
        if (!scheduler) {
            std::cerr << "No IO scheduler available\n";
            return 1;
        }

        scheduler->spawn(sendEchoRequest(runtime, url, message));

        // 等待一段时间让请求完成
        std::this_thread::sleep_for(std::chrono::seconds(3));

        // 停止 Runtime
        runtime.stop();
        std::cout << "Runtime stopped\n";

    } catch (const std::exception& e) {
        std::cerr << "Client error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
