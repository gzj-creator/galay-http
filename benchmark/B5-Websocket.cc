/**
 * @file B5-Websocket.cc
 * @brief WebSocket 服务器压测程序
 * @details 配合 B4-WebsocketClient 进行 WebSocket 性能测试
 */

#include "galay-http/kernel/http/HttpServer.h"
#include "galay-http/kernel/websocket/WsUpgrade.h"
#include "galay-http/kernel/websocket/WsConn.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/kernel/http/HttpLog.h"
#include <iostream>
#include <atomic>
#include <signal.h>

using namespace galay::http;
using namespace galay::websocket;
using namespace galay::kernel;

// 统计信息
std::atomic<int> total_connections{0};
std::atomic<int> total_messages{0};
std::atomic<long long> total_bytes{0};
std::atomic<bool> g_running{true};

void signalHandler(int) {
    g_running = false;
}

/**
 * @brief WebSocket 连接处理协程
 */
Coroutine handleWebSocketConnection(WsConn& ws_conn) {
    total_connections++;

    auto reader = ws_conn.getReader();
    auto writer = ws_conn.getWriter();

    // 发送欢迎消息
    co_await writer.sendText("Welcome to WebSocket Benchmark Server!");

    // 消息循环
    while (true) {
        std::string message;
        WsOpcode opcode;

        auto result = co_await reader.getMessage(message, opcode);

        if (!result.has_value()) {
            // 连接错误
            break;
        }

        if (!result.value()) {
            // 消息未完成，继续读取
            continue;
        }

        // 处理不同类型的消息
        if (opcode == WsOpcode::Text || opcode == WsOpcode::Binary) {
            total_messages++;
            total_bytes += message.size();

            // 回显消息
            if (opcode == WsOpcode::Text) {
                co_await writer.sendText(message);
            } else {
                co_await writer.sendBinary(message);
            }

        } else if (opcode == WsOpcode::Ping) {
            // 响应 Ping
            co_await writer.sendPong(message);

        } else if (opcode == WsOpcode::Close) {
            // 客户端关闭连接
            co_await writer.sendClose();
            break;
        }
    }

    co_await ws_conn.close();
    co_return;
}

/**
 * @brief HTTP 请求处理协程
 */
Coroutine handleHttpRequest(HttpConn conn) {
    auto reader = conn.getReader();
    HttpRequest request;

    auto read_result = co_await reader.getRequest(request);
    if (!read_result) {
        co_await conn.close();
        co_return;
    }

    // 检查是否是 WebSocket 升级请求
    if (request.header().uri() == "/ws" || request.header().uri() == "/") {
        auto upgrade_result = WsUpgrade::handleUpgrade(request);

        if (!upgrade_result.success) {
            auto writer = conn.getWriter();
            co_await writer.sendResponse(upgrade_result.response);
            co_await conn.close();
            co_return;
        }

        // 发送升级响应
        auto writer = conn.getWriter();
        auto send_result = co_await writer.sendResponse(upgrade_result.response);

        if (!send_result) {
            co_await conn.close();
            co_return;
        }

        // 升级到 WebSocket
        WsReaderSetting reader_setting;
        reader_setting.max_frame_size = 1024 * 1024;
        reader_setting.max_message_size = 10 * 1024 * 1024;

        WsWriterSetting writer_setting;

        WsConn ws_conn(
            std::move(conn),
            true  // is_server
        );

        co_await handleWebSocketConnection(ws_conn).wait();
    } else {
        // 非 WebSocket 请求，返回 404
        HttpResponse response;
        response.header().version() = HttpVersion::HttpVersion_1_1;
        response.header().code() = HttpStatusCode::NotFound_404;
        response.setBodyStr("Not Found");

        auto writer = conn.getWriter();
        co_await writer.sendResponse(response);
        co_await conn.close();
    }

    co_return;
}

int main(int argc, char* argv[]) {
    uint16_t port = 8080;
    if (argc >= 2) {
        port = std::atoi(argv[1]);
    }

    std::cout << "========================================" << std::endl;
    std::cout << "WebSocket Benchmark Server" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Port: " << port << std::endl;
    std::cout << "WebSocket endpoint: ws://localhost:" << port << "/ws" << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // 设置信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try {
        HttpServerConfig config;
        config.host = "0.0.0.0";
        config.port = port;
        config.io_scheduler_count = 4;
        config.compute_scheduler_count = 0;

        HttpServer server(config);

        HTTP_LOG_INFO("WebSocket benchmark server starting on {}:{}", config.host, config.port);

        server.start(handleHttpRequest);

        std::cout << "Server started successfully!\n" << std::endl;

        // 等待停止信号
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "\nShutting down..." << std::endl;
        server.stop();

        // 打印统计信息
        std::cout << "\n========================================" << std::endl;
        std::cout << "Benchmark Statistics:" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "Total connections: " << total_connections.load() << std::endl;
        std::cout << "Total messages: " << total_messages.load() << std::endl;
        std::cout << "Total bytes: " << total_bytes.load() << " ("
                  << (total_bytes.load() / 1024.0 / 1024.0) << " MB)" << std::endl;
        std::cout << "========================================" << std::endl;

        std::cout << "Server stopped." << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
