/**
 * @file T18-WsServer.cc
 * @brief WebSocket 服务器测试程序
 * @details 基于 HttpServer 实现 WebSocket 服务器测试
 *
 * 使用方法:
 *   ./test/T18-WsServer [port]
 *   默认端口: 8080
 */

#include "galay-http/kernel/http/HttpServer.h"
#include "galay-http/kernel/websocket/WsUpgrade.h"
#include "galay-http/kernel/websocket/WsConn.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/kernel/http/HttpLog.h"
#include "kernel/websocket/WsWriterSetting.h"
#include <iostream>
#include <atomic>
#include <csignal>
#include <utility>

using namespace galay::http;
using namespace galay::websocket;
using namespace galay::kernel;

static std::atomic<bool> g_running{true};
static std::atomic<uint64_t> g_message_count{0};
static std::atomic<uint64_t> g_connection_count{0};

void signalHandler(int) {
    g_running = false;
}

/**
 * @brief 处理 WebSocket 连接
 */
Coroutine handleWebSocketConnection(WsConn& ws_conn) {
    g_connection_count++;
    HTTP_LOG_INFO("WebSocket connection #{} established", g_connection_count.load());

    auto reader = ws_conn.getReader();
    auto writer = ws_conn.getWriter(WsWriterSetting::byServer());

    // 发送欢迎消息（循环等待直到完成）
    while (true) {
        auto send_result = co_await writer.sendText("Welcome to WebSocket Test Server!");
        if (!send_result) {
            HTTP_LOG_ERROR("Failed to send welcome message: {}", send_result.error().message());
            co_return;
        }
        if (send_result.value()) break;
    }

    // 消息循环
    while (true) {
        std::string message;
        WsOpcode opcode;

        // 循环等待接收消息
        bool recv_success = false;
        while (true) {
            auto result = co_await reader.getMessage(message, opcode);

            if (!result.has_value()) {
                WsError error = result.error();
                if (error.code() == kWsConnectionClosed) {
                    HTTP_LOG_INFO("WebSocket connection closed by peer");
                    co_await ws_conn.close();
                    co_return;
                }
                HTTP_LOG_ERROR("Failed to read message: {}", error.message());
                co_await ws_conn.close();
                co_return;
            }

            if (result.value()) {
                recv_success = true;
                break;
            }
        }

        if (!recv_success) {
            break;
        }

        g_message_count++;

        // 处理不同类型的消息
        if (opcode == WsOpcode::Ping) {
            HTTP_LOG_INFO("Received Ping, sending Pong");
            while (true) {
                auto pong_result = co_await writer.sendPong(message);
                if (!pong_result) {
                    HTTP_LOG_ERROR("Failed to send Pong: {}", pong_result.error().message());
                    co_await ws_conn.close();
                    co_return;
                }
                if (pong_result.value()) break;
            }
        }
        else if (opcode == WsOpcode::Pong) {
            HTTP_LOG_INFO("Received Pong");
        }
        else if (opcode == WsOpcode::Close) {
            HTTP_LOG_INFO("Received Close frame");
            while (true) {
                auto close_result = co_await writer.sendClose();
                if (!close_result || close_result.value()) break;
            }
            break;
        }
        else if (opcode == WsOpcode::Text) {
            HTTP_LOG_INFO("Message #{}: {}", g_message_count.load(), message);

            // Echo back
            std::string echo_msg = "Echo: " + message;
            while (true) {
                auto echo_result = co_await writer.sendText(echo_msg);
                if (!echo_result) {
                    HTTP_LOG_ERROR("Failed to send echo: {}", echo_result.error().message());
                    co_await ws_conn.close();
                    co_return;
                }
                if (echo_result.value()) break;
            }
        }
        else if (opcode == WsOpcode::Binary) {
            HTTP_LOG_INFO("Binary message #{}, size: {}", g_message_count.load(), message.size());

            // Echo back binary
            while (true) {
                auto echo_result = co_await writer.sendBinary(message);
                if (!echo_result) {
                    HTTP_LOG_ERROR("Failed to send binary echo: {}", echo_result.error().message());
                    co_await ws_conn.close();
                    co_return;
                }
                if (echo_result.value()) break;
            }
        }
    }

    HTTP_LOG_INFO("Closing WebSocket connection");
    co_await ws_conn.close();
    co_return;
}

/**
 * @brief HTTP 请求处理器
 */
Coroutine handleHttpRequest(HttpConn conn) {
    auto reader = conn.getReader();
    HttpRequest request;

    auto read_result = co_await reader.getRequest(request);
    if (!read_result) {
        HTTP_LOG_ERROR("Failed to read HTTP request: {}", read_result.error().message());
        co_await conn.close();
        co_return;
    }

    HTTP_LOG_INFO("Received {} {}", httpMethodToString(request.header().method()), request.header().uri());

    // 检查是否是 WebSocket 升级请求
    if (request.header().uri() == "/ws" || request.header().uri() == "/") {
        auto upgrade_result = WsUpgrade::handleUpgrade(request);

        if (!upgrade_result.success) {
            HTTP_LOG_ERROR("WebSocket upgrade failed: {}", upgrade_result.error_message);

            auto writer = conn.getWriter();
            co_await writer.sendResponse(upgrade_result.response);
            co_await conn.close();
            co_return;
        }

        HTTP_LOG_INFO("WebSocket upgrade successful");

        // 发送升级响应
        auto writer = conn.getWriter();
        auto send_result = co_await writer.sendResponse(upgrade_result.response);

        if (!send_result) {
            HTTP_LOG_ERROR("Failed to send upgrade response: {}", send_result.error().message());
            co_await conn.close();
            co_return;
        }

        WsConn ws_conn = WsConn::from(std::move(conn), true);

        co_await handleWebSocketConnection(ws_conn).wait();
    }
    else {
        // 普通 HTTP 请求
        HttpResponse response;
        response.header().version() = HttpVersion::HttpVersion_1_1;
        response.header().code() = HttpStatusCode::OK_200;
        response.header().headerPairs().addHeaderPair("Content-Type", "text/html");

        std::string body = "<html><body><h1>WebSocket Test Server</h1><p>Connect to /ws for WebSocket</p></body></html>";
        response.setBodyStr(std::move(body));

        auto writer = conn.getWriter();
        co_await writer.sendResponse(response);
        co_await conn.close();
    }

    co_return;
}

int main(int argc, char* argv[]) {
    uint16_t port = 8080;
    if (argc > 1) {
        port = std::atoi(argv[1]);
    }

    std::cout << "========================================\n";
    std::cout << "WebSocket Server Test\n";
    std::cout << "========================================\n";
    std::cout << "Port: " << port << "\n";
    std::cout << "WebSocket endpoint: ws://localhost:" << port << "/ws\n";
    std::cout << "Test command: ./test/T19-WsClient localhost " << port << "\n";
    std::cout << "Press Ctrl+C to stop\n";
    std::cout << "========================================\n\n";

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try {
        HttpServerConfig config;
        config.host = "0.0.0.0";
        config.port = port;
        config.io_scheduler_count = 4;
        config.compute_scheduler_count = 0;

        HttpServer server(config);

        HTTP_LOG_INFO("WebSocket test server starting on {}:{}", config.host, config.port);

        server.start(handleHttpRequest);

        std::cout << "Server started successfully!\n\n";

        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "\n\nShutting down...\n";
        std::cout << "Total connections: " << g_connection_count << "\n";
        std::cout << "Total messages: " << g_message_count << "\n";

        server.stop();
        std::cout << "Server stopped.\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
