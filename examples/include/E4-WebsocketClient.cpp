/**
 * @file E4-WebsocketClient.cc
 * @brief WebSocket 客户端示例
 * @details 展示如何使用 WsClient 连接到 WebSocket 服务器并进行双向通信
 */

#include <iostream>
#include "galay-http/kernel/websocket/WsClient.h"
#include "galay-http/kernel/websocket/WsWriterSetting.h"
#include "galay-kernel/common/Log.h"
#include "galay-kernel/kernel/Runtime.h"

using namespace galay::http;
using namespace galay::websocket;
using namespace galay::kernel;

/**
 * @brief WebSocket 客户端协程
 */
Coroutine runWebSocketClient(const std::string& url) {
    LogInfo("Connecting to {}", url);

    // 1. 创建 WsClient
    auto client = WsClientBuilder().build();

    // 2. 连接到服务器
    auto connect_result = co_await client.connect(url);
    if (!connect_result) {
        LogError("Failed to connect: {}", connect_result.error().message());
        co_return;
    }
    LogInfo("TCP connection established");

    // 3. 获取 Session
    WsReaderSetting reader_setting;
    reader_setting.max_frame_size = 1024 * 1024;  // 1MB
    reader_setting.max_message_size = 10 * 1024 * 1024;  // 10MB

    auto session = client.getSession(WsWriterSetting::byClient(), 8192, reader_setting);

    // 4. 执行 WebSocket 升级
    auto upgrader = session.upgrade();
    auto result = co_await upgrader();
    if (!result) {
        LogError("Upgrade failed: {}", result.error().message());
        co_return;
    }
    if (!result.value()) {
        LogError("Upgrade failed: incomplete result");
        co_return;
    }
    LogInfo("WebSocket upgrade successful");

    // 5. 获取 Reader 和 Writer
    auto reader = session.getReader();
    auto writer = session.getWriter();

    // 6. 读取欢迎消息
    LogInfo("Waiting for welcome message");
    std::string welcome_message;
    WsOpcode welcome_opcode;

    while (true) {
        auto result = co_await reader.getMessage(welcome_message, welcome_opcode);
        if (!result) {
            LogError("Failed to receive welcome message: {}", result.error().message());
            co_await client.close();
            co_return;
        }
        if (result.value()) {
            break;  // 消息接收完成
        }
    }
    LogInfo("Received welcome message: {}", welcome_message);

    // 7. 发送测试消息
    std::vector<std::string> test_messages = {
        "Hello from WebSocket client!",
        "This is message number 2",
        "Testing WebSocket communication",
        "Final test message"
    };

    for (const auto& msg : test_messages) {
        // 发送消息
        LogInfo("Sending message: {}", msg);
        while (true) {
            auto send_result = co_await writer.sendText(msg);
            if (!send_result) {
                LogError("Failed to send message: {}", send_result.error().message());
                co_await client.close();
                co_return;
            }
            if (send_result.value()) {
                break;
            }
        }

        // 接收回显消息
        std::string echo_message;
        WsOpcode echo_opcode;

        while (true) {
            auto result = co_await reader.getMessage(echo_message, echo_opcode);
            if (!result) {
                if (result.error().code() == kWsConnectionClosed) {
                    LogInfo("WebSocket connection closed by server");
                    co_await client.close();
                    co_return;
                }
                LogError("Failed to read message: {}", result.error().message());
                co_await client.close();
                co_return;
            }

            if (!result.value()) {
                continue;  // 消息不完整，继续读取
            }

            // 处理不同类型的消息
            if (echo_opcode == WsOpcode::Ping) {
                LogInfo("Received Ping, sending Pong");
                while (true) {
                    auto pong_result = co_await writer.sendPong(echo_message);
                    if (!pong_result) {
                        LogError("Failed to send Pong: {}", pong_result.error().message());
                        co_await client.close();
                        co_return;
                    }
                    if (pong_result.value()) {
                        break;
                    }
                }
                continue;
            }
            else if (echo_opcode == WsOpcode::Pong) {
                LogInfo("Received Pong");
                continue;
            }
            else if (echo_opcode == WsOpcode::Close) {
                LogInfo("Received Close frame");
                while (true) {
                    auto close_result = co_await writer.sendClose();
                    if (!close_result || close_result.value()) {
                        break;
                    }
                }
                co_await client.close();
                co_return;
            }
            else if (echo_opcode == WsOpcode::Text || echo_opcode == WsOpcode::Binary) {
                break;  // 收到数据消息
            }
        }

        LogInfo("Received echo: {}", echo_message);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // 8. 发送 Ping 测试
    LogInfo("Sending Ping frame");
    while (true) {
        auto ping_result = co_await writer.sendPing("ping");
        if (!ping_result) {
            LogError("Failed to send Ping: {}", ping_result.error().message());
            break;
        }
        if (ping_result.value()) {
            LogInfo("Ping sent successfully");
            break;
        }
    }

    // 等待 Pong 响应
    std::string pong_message;
    WsOpcode pong_opcode;
    while (true) {
        auto result = co_await reader.getMessage(pong_message, pong_opcode);
        if (!result) {
            LogError("Failed to receive Pong: {}", result.error().message());
            break;
        }
        if (!result.value()) {
            continue;
        }
        if (pong_opcode == WsOpcode::Pong) {
            LogInfo("Received Pong response");
            break;
        }
    }

    // 9. 关闭连接
    LogInfo("Closing WebSocket connection");
    while (true) {
        auto close_result = co_await writer.sendClose();
        if (!close_result || close_result.value()) {
            break;
        }
    }
    co_await client.close();
    LogInfo("WebSocket client finished");
    co_return;
}

int main(int argc, char* argv[]) {
    // 解析命令行参数
    std::string url = "ws://127.0.0.1:8080/ws";

    if (argc > 1) {
        url = argv[1];
    }

    std::cout << "========================================\n";
    std::cout << "WebSocket Client Example\n";
    std::cout << "========================================\n";
    std::cout << "URL: " << url << "\n";
    std::cout << "========================================\n\n";

    try {
        // 创建 Runtime
        Runtime runtime(1, 0);
        runtime.start();

        LogInfo("Runtime started");

        // 获取调度器并启动 WebSocket 客户端协程
        auto* scheduler = runtime.getNextIOScheduler();
        if (!scheduler) {
            LogError("No IO scheduler available");
            return 1;
        }

        scheduler->spawn(runWebSocketClient(url));

        // 等待足够的时间让 WebSocket 通信完成
        std::this_thread::sleep_for(std::chrono::seconds(10));

        // 停止 Runtime
        runtime.stop();
        LogInfo("Runtime stopped");

    } catch (const std::exception& e) {
        LogError("Client error: {}", e.what());
        return 1;
    }

    return 0;
}
