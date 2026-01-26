/**
 * @file example_wsclient_simple.cc
 * @brief 使用 WsClient 的简单示例
 * @details 展示如何使用 WsClient 连接到 WebSocket 服务器
 */

#include <iostream>
#include <chrono>
#include "galay-http/kernel/websocket/WsClient.h"
#include "galay-http/kernel/http/HttpLog.h"
#include "galay-kernel/kernel/Runtime.h"

using namespace galay::websocket;
using namespace galay::kernel;
using namespace std::chrono_literals;

/**
 * @brief WebSocket 客户端示例
 */
Coroutine websocketClientExample(const std::string& url) {
    HTTP_LOG_INFO("=== WebSocket Client Example ===");
    HTTP_LOG_INFO("Connecting to: {}", url);

    // 创建 WsClient
    WsClient client;

    // 连接到服务器（自动解析 ws:// URL）
    auto connect_result = co_await client.connect(url);
    if (!connect_result) {
        HTTP_LOG_ERROR("Failed to connect: {}", connect_result.error().message());
        co_return;
    }

    HTTP_LOG_INFO("Connected successfully!");

    // WebSocket 握手升级
    HTTP_LOG_INFO("Starting WebSocket upgrade...");
    while (true) {
        auto upgrade_result = co_await client.upgrade();
        if (!upgrade_result.has_value()) {
            HTTP_LOG_ERROR("Upgrade failed: {}", upgrade_result.error().message());
            co_return;
        }
        if (upgrade_result.value()) {
            // 升级成功
            HTTP_LOG_INFO("WebSocket upgrade successful!");
            break;
        }
        // 继续等待
    }

    // 接收欢迎消息
    {
        std::string message;
        WsOpcode opcode;

        bool message_complete = false;
        while (!message_complete) {
            auto result = co_await client.getWsReader().getMessage(message, opcode);
            if (!result.has_value()) {
                HTTP_LOG_ERROR("Failed to receive welcome message: {}", result.error().message());
                co_await client.close();
                co_return;
            }
            message_complete = result.value();
        }

        HTTP_LOG_INFO("Received welcome: {}", message);
    }

    // 发送测试消息
    std::vector<std::string> test_messages = {
        "Hello from WsClient!",
        "This is message 2",
        "Testing WebSocket",
        "Final message"
    };

    for (const auto& msg : test_messages) {
        // 发送消息
        HTTP_LOG_INFO("Sending: {}", msg);
        auto send_result = co_await client.getWsWriter().sendText(msg);
        if (!send_result) {
            HTTP_LOG_ERROR("Failed to send: {}", send_result.error().message());
            break;
        }

        // 接收回显
        std::string echo_message;
        WsOpcode echo_opcode;

        bool echo_complete = false;
        while (!echo_complete) {
            auto result = co_await client.getWsReader().getMessage(echo_message, echo_opcode);
            if (!result.has_value()) {
                HTTP_LOG_ERROR("Failed to receive: {}", result.error().message());
                co_await client.close();
                co_return;
            }

            if (!result.value()) {
                continue;
            }

            // 处理控制帧
            if (echo_opcode == WsOpcode::Ping) {
                HTTP_LOG_INFO("Received Ping, sending Pong");
                co_await client.getWsWriter().sendPong(echo_message);
                continue;
            } else if (echo_opcode == WsOpcode::Close) {
                HTTP_LOG_INFO("Received Close");
                co_await client.getWsWriter().sendClose();
                co_await client.close();
                co_return;
            } else if (echo_opcode == WsOpcode::Text || echo_opcode == WsOpcode::Binary) {
                echo_complete = true;
            }
        }

        HTTP_LOG_INFO("Received echo: {}", echo_message);

        // 等待一小段时间
        std::this_thread::sleep_for(500ms);
    }

    // 发送 Ping 测试
    HTTP_LOG_INFO("Sending Ping");
    auto ping_result = co_await client.getWsWriter().sendPing("ping");
    if (ping_result) {
        // 等待 Pong
        std::string pong_message;
        WsOpcode pong_opcode;

        bool pong_received = false;
        while (!pong_received) {
            auto result = co_await client.getWsReader().getMessage(pong_message, pong_opcode);
            if (!result.has_value() || !result.value()) {
                continue;
            }

            if (pong_opcode == WsOpcode::Pong) {
                HTTP_LOG_INFO("Received Pong");
                pong_received = true;
            }
        }
    }

    // 关闭连接
    HTTP_LOG_INFO("Closing connection");
    co_await client.getWsWriter().sendClose();
    co_await client.close();

    HTTP_LOG_INFO("=== WebSocket Client Example Completed ===");
    co_return;
}

int main(int argc, char* argv[]) {
    // 解析命令行参数
    std::string url = "ws://127.0.0.1:8080/ws";

    if (argc > 1) {
        url = argv[1];
    }

    std::cout << "========================================\n";
    std::cout << "WebSocket Client Simple Example\n";
    std::cout << "========================================\n";
    std::cout << "URL: " << url << "\n";
    std::cout << "========================================\n\n";

    try {
        // 创建 Runtime
        Runtime runtime(LoadBalanceStrategy::ROUND_ROBIN, 1, 1);
        runtime.start();

        HTTP_LOG_INFO("Runtime started");

        // 获取调度器并启动客户端协程
        auto* scheduler = runtime.getNextIOScheduler();
        if (!scheduler) {
            HTTP_LOG_ERROR("No IO scheduler available");
            return 1;
        }

        scheduler->spawn(websocketClientExample(url));

        // 等待足够的时间让通信完成
        std::this_thread::sleep_for(std::chrono::seconds(10));

        // 停止 Runtime
        runtime.stop();
        HTTP_LOG_INFO("Runtime stopped");

    } catch (const std::exception& e) {
        HTTP_LOG_ERROR("Client error: {}", e.what());
        return 1;
    }

    return 0;
}
