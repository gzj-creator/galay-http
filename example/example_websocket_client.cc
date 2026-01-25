/**
 * @file example_websocket_client.cc
 * @brief WebSocket 客户端完整示例
 * @details 展示如何连接到 WebSocket 服务器并进行双向通信
 */

#include <iostream>
#include <memory>
#include <random>
#include "galay-http/kernel/http/HttpClient.h"
#include "galay-http/kernel/websocket/WsUpgrade.h"
#include "galay-http/kernel/websocket/WsConn.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/utils/Http1_1RequestBuilder.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/common/Log.h"
#include "galay-kernel/kernel/Runtime.h"
#include <galay-utils/algorithm/Base64.hpp>

using namespace galay::http;
using namespace galay::websocket;
using namespace galay::kernel;
using namespace galay::async;
using namespace galay::utils;

/**
 * @brief 生成随机的 WebSocket Key
 * @return Base64 编码的 16 字节随机数
 */
std::string generateWebSocketKey() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);

    unsigned char random_bytes[16];
    for (int i = 0; i < 16; i++) {
        random_bytes[i] = static_cast<unsigned char>(dis(gen));
    }

    return Base64Util::Base64Encode(random_bytes, 16);
}

/**
 * @brief 处理 WebSocket 连接
 * @param ws_conn WebSocket 连接
 */
Coroutine handleWebSocketClient(WsConn ws_conn) {
    LogInfo("WebSocket connection established");

    // 获取 Reader 和 Writer
    auto reader = ws_conn.getReader();
    auto writer = ws_conn.getWriter();

    // 读取欢迎消息
    LogInfo("Waiting for welcome message");
    std::string welcome_message;
    WsOpcode welcome_opcode;

    // 循环读取直到收到完整消息
    bool message_complete = false;
    while (!message_complete) {
        auto welcome_result = co_await reader.getMessage(welcome_message, welcome_opcode);

        if (!welcome_result.has_value()) {
            LogError("Failed to receive welcome message: {}", welcome_result.error().message());
            co_await ws_conn.close();
            co_return;
        }

        message_complete = welcome_result.value();
    }

    LogInfo("Received welcome message: {}", welcome_message);

    // 发送测试消息
    std::vector<std::string> test_messages = {
        "Hello from WebSocket client!",
        "This is message number 2",
        "Testing WebSocket communication",
        "Final test message"
    };

    for (const auto& msg : test_messages) {
        // 发送消息
        LogInfo("Sending message: {}", msg);
        auto send_result = co_await writer.sendText(msg);
        if (!send_result) {
            LogError("Failed to send message: {}", send_result.error().message());
            break;
        }

        // 接收回显消息（可能会收到控制帧）
        std::string echo_message;
        WsOpcode echo_opcode;

        // 循环读取直到收到完整的数据消息
        bool echo_complete = false;
        while (!echo_complete) {
            auto echo_result = co_await reader.getMessage(echo_message, echo_opcode);

            if (!echo_result.has_value()) {
                WsError error = echo_result.error();
                if (error.code() == kWsConnectionClosed) {
                    LogInfo("WebSocket connection closed by server");
                    co_await ws_conn.close();
                    co_return;
                }
                LogError("Failed to read message: {}", error.message());
                co_await ws_conn.close();
                co_return;
            }

            if (!echo_result.value()) {
                // 消息不完整，继续读取
                continue;
            }

            // 根据 opcode 处理不同类型的消息
            if (echo_opcode == WsOpcode::Ping) {
                // 收到 Ping，发送 Pong 响应
                LogInfo("Received Ping frame, sending Pong response");
                auto pong_result = co_await writer.sendPong(echo_message);
                if (!pong_result) {
                    LogError("Failed to send Pong: {}", pong_result.error().message());
                    co_await ws_conn.close();
                    co_return;
                }
                LogInfo("Pong sent successfully");
                // 继续读取数据消息
                continue;
            }
            else if (echo_opcode == WsOpcode::Pong) {
                LogInfo("Received Pong frame");
                // 继续读取数据消息
                continue;
            }
            else if (echo_opcode == WsOpcode::Close) {
                LogInfo("Received Close frame");
                co_await writer.sendClose();
                co_await ws_conn.close();
                co_return;
            }
            else if (echo_opcode == WsOpcode::Text || echo_opcode == WsOpcode::Binary) {
                // 收到数据消息
                echo_complete = true;
            }
        }

        LogInfo("Received echo: {}", echo_message);

        // 等待一小段时间
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // 发送 Ping 帧测试
    LogInfo("Sending Ping frame");
    auto ping_result = co_await writer.sendPing("ping");
    if (ping_result) {
        LogInfo("Ping sent successfully");

        // 等待 Pong 响应
        std::string pong_message;
        WsOpcode pong_opcode;

        bool pong_received = false;
        while (!pong_received) {
            auto pong_result = co_await reader.getMessage(pong_message, pong_opcode);

            if (!pong_result.has_value()) {
                LogError("Failed to receive Pong: {}", pong_result.error().message());
                break;
            }

            if (!pong_result.value()) {
                continue;
            }

            if (pong_opcode == WsOpcode::Pong) {
                LogInfo("Received Pong response");
                pong_received = true;
            }
        }
    }

    // 关闭连接
    LogInfo("Closing WebSocket connection");
    co_await writer.sendClose();
    co_await ws_conn.close();
    LogInfo("WebSocket client finished");
    co_return;
}

/**
 * @brief 连接到 WebSocket 服务器
 * @param runtime Runtime 引用
 * @param host 服务器地址
 * @param port 服务器端口
 * @param path WebSocket 路径
 */
Coroutine connectToWebSocket(Runtime& runtime, const std::string& host, int port, const std::string& path) {
    LogInfo("Connecting to WebSocket server at {}:{}{}...", host, port, path);

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

    LogInfo("TCP connection established");

    // 创建 HttpClient
    HttpClient client(std::move(socket));

    // 生成 WebSocket Key
    std::string ws_key = generateWebSocketKey();
    LogInfo("Generated WebSocket-Key: {}", ws_key);

    // 构造 WebSocket 升级请求
    auto request = Http1_1RequestBuilder::get(path)
        .host(host + ":" + std::to_string(port))
        .header("Connection", "Upgrade")
        .header("Upgrade", "websocket")
        .header("Sec-WebSocket-Version", "13")
        .header("Sec-WebSocket-Key", ws_key)
        .build();

    LogInfo("Sending WebSocket upgrade request...");

    // 发送升级请求
    auto writer = client.getWriter();
    auto send_result = co_await writer.sendRequest(request);
    if (!send_result) {
        LogError("Failed to send upgrade request: {}", send_result.error().message());
        co_return;
    }

    LogInfo("Upgrade request sent, waiting for response...");

    // 接收升级响应
    auto reader = client.getReader();
    HttpResponse response;
    auto recv_result = co_await reader.getResponse(response);
    if (!recv_result) {
        LogError("Failed to receive upgrade response: {}", recv_result.error().message());
        co_return;
    }

    // 检查响应状态码
    if (response.header().code() != HttpStatusCode::SwitchingProtocol_101) {
        LogError("WebSocket upgrade failed. Status: {} {}",
                static_cast<int>(response.header().code()),
                httpStatusCodeToString(response.header().code()));
        LogError("Response body: {}", response.getBodyStr());
        co_return;
    }

    // 验证 Sec-WebSocket-Accept
    if (!response.header().headerPairs().hasKey("Sec-WebSocket-Accept")) {
        LogError("Missing Sec-WebSocket-Accept header in response");
        co_return;
    }

    std::string accept_key = response.header().headerPairs().getValue("Sec-WebSocket-Accept");
    std::string expected_accept = WsUpgrade::generateAcceptKey(ws_key);

    if (accept_key != expected_accept) {
        LogError("Invalid Sec-WebSocket-Accept value");
        LogError("Expected: {}", expected_accept);
        LogError("Received: {}", accept_key);
        co_return;
    }

    LogInfo("WebSocket upgrade successful!");
    LogInfo("Sec-WebSocket-Accept verified");

    // 创建 WebSocket 连接
    WsReaderSetting reader_setting;
    reader_setting.max_frame_size = 1024 * 1024;  // 1MB
    reader_setting.max_message_size = 10 * 1024 * 1024;  // 10MB

    WsWriterSetting writer_setting;
    // 注意：不需要手动设置 use_mask，WsConn 会根据 is_server 参数自动设置

    WsConn ws_conn(
        std::move(client.socket()),
        std::move(client.ringBuffer()),
        reader_setting,
        writer_setting,
        false  // is_server = false (客户端，WsConn 会自动设置 use_mask = true)
    );

    LogInfo("=== WsConn created, starting WebSocket communication ===");

    // 处理 WebSocket 通信
    try {
        co_await handleWebSocketClient(std::move(ws_conn)).wait();
        LogInfo("=== WebSocket client communication completed successfully ===");
    } catch (const std::exception& e) {
        LogError("Exception in WebSocket communication: {}", e.what());
    }

    LogInfo("=== WebSocket client connection finished ===");
    co_return;
}

int main(int argc, char* argv[]) {
    // 解析命令行参数
    std::string host = "127.0.0.1";
    int port = 8080;
    std::string path = "/ws";

    if (argc > 1) {
        host = argv[1];
    }
    if (argc > 2) {
        port = std::atoi(argv[2]);
    }
    if (argc > 3) {
        path = argv[3];
    }

    std::cout << "========================================\n";
    std::cout << "WebSocket Client Example\n";
    std::cout << "========================================\n";
    std::cout << "Server: " << host << ":" << port << "\n";
    std::cout << "Path: " << path << "\n";
    std::cout << "WebSocket URL: ws://" << host << ":" << port << path << "\n";
    std::cout << "========================================\n\n";

    try {
        // 创建 Runtime
        Runtime runtime(LoadBalanceStrategy::ROUND_ROBIN, 1, 1);
        runtime.start();

        LogInfo("Runtime started");

        // 获取调度器并启动 WebSocket 连接协程
        auto* scheduler = runtime.getNextIOScheduler();
        if (!scheduler) {
            LogError("No IO scheduler available");
            return 1;
        }

        scheduler->spawn(connectToWebSocket(runtime, host, port, path));

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
