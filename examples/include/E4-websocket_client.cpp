/**
 * @file E4-WebsocketClient.cc
 * @brief WebSocket 客户端示例
 * @details 展示如何使用 WsClient 连接到 WebSocket 服务器并进行双向通信
 */

#include <iostream>
#include "galay-http/kernel/websocket/WsClient.h"
#include "galay-http/kernel/websocket/WsWriterSetting.h"
#include "galay-http/kernel/http/HttpLog.h"
#include "galay-kernel/kernel/Runtime.h"

using namespace galay::http;
using namespace galay::websocket;
using namespace galay::kernel;

/**
 * @brief WebSocket 客户端协程
 */
Task<bool> runWebSocketClient(const std::string& url) {
    HTTP_LOG_INFO("Connecting to {}", url);

    // 1. 创建 WsClient
    auto client = WsClientBuilder().build();

    // 2. 连接到服务器
    auto connect_result = co_await client.connect(url);
    if (!connect_result) {
        HTTP_LOG_ERROR("Failed to connect: {}", connect_result.error().message());
        co_return false;
    }
    HTTP_LOG_INFO("TCP connection established");

    // 3. 获取 Session
    WsReaderSetting reader_setting;
    reader_setting.max_frame_size = 1024 * 1024;  // 1MB
    reader_setting.max_message_size = 10 * 1024 * 1024;  // 10MB

    auto session = client.getSession(WsWriterSetting::byClient(), 8192, reader_setting);

    // 4. 执行 WebSocket 升级
    auto upgrader = session.upgrade();
    auto result = co_await upgrader();
    if (!result) {
        HTTP_LOG_ERROR("Upgrade failed: {}", result.error().message());
        co_return false;
    }
    if (!result.value()) {
        HTTP_LOG_ERROR("Upgrade failed: incomplete result");
        co_return false;
    }
    HTTP_LOG_INFO("WebSocket upgrade successful");

    // 5. 获取 Reader 和 Writer
    auto reader = session.getReader();
    auto writer = session.getWriter();

    // 6. 读取欢迎消息
    HTTP_LOG_INFO("Waiting for welcome message");
    std::string welcome_message;
    WsOpcode welcome_opcode;

    while (true) {
        auto result = co_await reader.getMessage(welcome_message, welcome_opcode);
        if (!result) {
            HTTP_LOG_ERROR("Failed to receive welcome message: {}", result.error().message());
            co_await client.close();
            co_return false;
        }
        if (result.value()) {
            break;  // 消息接收完成
        }
    }
    HTTP_LOG_INFO("Received welcome message: {}", welcome_message);
    std::cout << "Received welcome message: " << welcome_message << "\n";

    // 7. 发送测试消息
    std::vector<std::string> test_messages = {
        "Hello from WebSocket client!",
        "This is message number 2",
        "Testing WebSocket communication",
        "Final test message"
    };

    for (const auto& msg : test_messages) {
        // 发送消息
        HTTP_LOG_INFO("Sending message: {}", msg);
        while (true) {
            auto send_result = co_await writer.sendText(msg);
            if (!send_result) {
                HTTP_LOG_ERROR("Failed to send message: {}", send_result.error().message());
                co_await client.close();
                co_return false;
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
                    HTTP_LOG_INFO("WebSocket connection closed by server");
                    co_await client.close();
                    co_return false;
                }
                HTTP_LOG_ERROR("Failed to read message: {}", result.error().message());
                co_await client.close();
                co_return false;
            }

            if (!result.value()) {
                continue;  // 消息不完整，继续读取
            }

            // 处理不同类型的消息
            if (echo_opcode == WsOpcode::Ping) {
                HTTP_LOG_INFO("Received Ping, sending Pong");
                while (true) {
                    auto pong_result = co_await writer.sendPong(echo_message);
                    if (!pong_result) {
                        HTTP_LOG_ERROR("Failed to send Pong: {}", pong_result.error().message());
                        co_await client.close();
                        co_return false;
                    }
                    if (pong_result.value()) {
                        break;
                    }
                }
                continue;
            }
            else if (echo_opcode == WsOpcode::Pong) {
                HTTP_LOG_INFO("Received Pong");
                continue;
            }
            else if (echo_opcode == WsOpcode::Close) {
                HTTP_LOG_INFO("Received Close frame");
                while (true) {
                    auto close_result = co_await writer.sendClose();
                    if (!close_result || close_result.value()) {
                        break;
                    }
                }
                co_await client.close();
                co_return false;
            }
            else if (echo_opcode == WsOpcode::Text || echo_opcode == WsOpcode::Binary) {
                break;  // 收到数据消息
            }
        }

        HTTP_LOG_INFO("Received echo: {}", echo_message);
        std::cout << "Received echo: " << echo_message << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // 8. 发送 Ping 测试
    HTTP_LOG_INFO("Sending Ping frame");
    while (true) {
        auto ping_result = co_await writer.sendPing("ping");
        if (!ping_result) {
            HTTP_LOG_ERROR("Failed to send Ping: {}", ping_result.error().message());
            co_await client.close();
            co_return false;
        }
        if (ping_result.value()) {
            HTTP_LOG_INFO("Ping sent successfully");
            break;
        }
    }

    // 等待 Pong 响应
    std::string pong_message;
    WsOpcode pong_opcode;
    while (true) {
        auto result = co_await reader.getMessage(pong_message, pong_opcode);
        if (!result) {
            HTTP_LOG_ERROR("Failed to receive Pong: {}", result.error().message());
            co_await client.close();
            co_return false;
        }
        if (!result.value()) {
            continue;
        }
        if (pong_opcode == WsOpcode::Pong) {
            HTTP_LOG_INFO("Received Pong response");
            std::cout << "Received Pong response\n";
            break;
        }
    }

    // 9. 关闭连接
    HTTP_LOG_INFO("Closing WebSocket connection");
    while (true) {
        auto close_result = co_await writer.sendClose();
        if (!close_result || close_result.value()) {
            break;
        }
    }
    co_await client.close();
    HTTP_LOG_INFO("WebSocket client finished");
    std::cout << "WebSocket client finished\n";
    co_return true;
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
        Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
        runtime.start();

        HTTP_LOG_INFO("Runtime started");

        auto join = runtime.spawn(runWebSocketClient(url));
        bool ok = join.join();

        // 停止 Runtime
        runtime.stop();
        HTTP_LOG_INFO("Runtime stopped");

        if (!ok) {
            return 1;
        }

    } catch (const std::exception& e) {
        HTTP_LOG_ERROR("Client error: {}", e.what());
        return 1;
    }

    return 0;
}
