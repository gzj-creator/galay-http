/**
 * @file B5-WebsocketServer.cc
 * @brief WebSocket 服务器压测程序（纯净版）
 * @details 配合 B6-WebsocketClient 进行 WebSocket 性能测试
 *          移除统计功能，由客户端负责统计
 */

#include "galay-http/kernel/http/HttpServer.h"
#include "galay-http/kernel/websocket/WsUpgrade.h"
#include "galay-http/kernel/websocket/WsConn.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/utils/Http1_1ResponseBuilder.h"
#include "galay-http/kernel/http/HttpLog.h"
#include "kernel/websocket/WsWriterSetting.h"
#include <iostream>
#include <csignal>

using namespace galay::http;
using namespace galay::websocket;
using namespace galay::kernel;

static volatile bool g_running = true;

void signalHandler(int) {
    g_running = false;
}

/**
 * @brief WebSocket 连接处理协程
 */
Coroutine handleWebSocketConnection(WsConn& ws_conn) {
    auto reader = ws_conn.getReader();
    auto writer = ws_conn.getWriter(WsWriterSetting::byServer());

    // 发送欢迎消息
    while (true) {
        auto res = co_await writer.sendText("Welcome to WebSocket Benchmark Server!");
        if(!res) {
            HTTP_LOG_ERROR("[ws] [welcome] [send-fail] [{}]", res.error().message());
            co_return;
        }
        if(res.value()) {
            break;
        }
    }

    // 消息循环
    while (true) {
        std::string message;
        WsOpcode opcode;

        auto result = co_await reader.getMessage(message, opcode);

        if (!result) {
            HTTP_LOG_ERROR("[ws] [read-error] [{}]", result.error().message());
            break;
        }

        if (!result.value()) {
            continue;
        }

        // 处理不同类型的消息
        if (opcode == WsOpcode::Text || opcode == WsOpcode::Binary) {
            // 回显消息
            if (opcode == WsOpcode::Text) {
                while (true) {
                    auto send_res = co_await writer.sendText(message);
                    if (!send_res) {
                        HTTP_LOG_ERROR("[ws] [echo-text] [send-fail] [{}]", send_res.error().message());
                        goto cleanup;
                    }
                    if (send_res.value()) {
                        break;
                    }
                }
            } else {
                while (true) {
                    auto send_res = co_await writer.sendBinary(message);
                    if (!send_res) {
                        HTTP_LOG_ERROR("[ws] [echo-binary] [send-fail] [{}]", send_res.error().message());
                        goto cleanup;
                    }
                    if (send_res.value()) {
                        break;
                    }
                }
            }

        } else if (opcode == WsOpcode::Ping) {
            // 响应 Ping
            while (true) {
                auto pong_res = co_await writer.sendPong(message);
                if (!pong_res) {
                    HTTP_LOG_ERROR("[ws] [pong] [send-fail] [{}]", pong_res.error().message());
                    goto cleanup;
                }
                if (pong_res.value()) {
                    break;
                }
            }

        } else if (opcode == WsOpcode::Close) {
            // 客户端关闭连接
            while (true) {
                auto close_res = co_await writer.sendClose();
                if (!close_res) {
                    HTTP_LOG_ERROR("[ws] [close] [send-fail] [{}]", close_res.error().message());
                    break;
                }
                if (close_res.value()) {
                    break;
                }
            }
            break;
        }
    }

cleanup:
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
        HTTP_LOG_ERROR("[http] [read-fail] [{}]", read_result.error().message());
        co_await conn.close();
        co_return;
    }

    // 检查是否是 WebSocket 升级请求
    if (request.header().uri() == "/ws" || request.header().uri() == "/") {
        auto upgrade_result = WsUpgrade::handleUpgrade(request);

        if (!upgrade_result.success) {
            HTTP_LOG_ERROR("[http] [ws-upgrade] [fail]");
            auto writer = conn.getWriter();
            co_await writer.sendResponse(upgrade_result.response);
            co_await conn.close();
            co_return;
        }

        // 发送升级响应
        auto writer = conn.getWriter();
        auto send_result = co_await writer.sendResponse(upgrade_result.response);

        if (!send_result) {
            HTTP_LOG_ERROR("[http] [ws-upgrade] [send-fail] [{}]", send_result.error().message());
            co_await conn.close();
            co_return;
        }

        WsConn ws_conn = WsConn::from(std::move(conn), true);
        co_await handleWebSocketConnection(ws_conn).wait();
    } else {
        // 非 WebSocket 请求，返回 404
        auto response = Http1_1ResponseBuilder()
            .status(HttpStatusCode::NotFound_404)
            .body("Not Found")
            .buildMove();

        auto writer = conn.getWriter();
        co_await writer.sendResponse(response);
        co_await conn.close();
    }

    co_return;
}

int main(int argc, char* argv[]) {
    // 禁用日志以获得最佳性能
    galay::http::HttpLogger::disable();

    uint16_t port = 8080;
    int io_threads = 4;

    if (argc > 1) {
        port = std::atoi(argv[1]);
    }
    if (argc > 2) {
        io_threads = std::atoi(argv[2]);
    }

    std::cout << "========================================\n";
    std::cout << "WebSocket Benchmark Server\n";
    std::cout << "========================================\n";
    std::cout << "Port: " << port << "\n";
    std::cout << "IO Threads: " << io_threads << "\n";
    std::cout << "WebSocket endpoint: ws://localhost:" << port << "/ws\n";
    std::cout << "Press Ctrl+C to stop\n";
    std::cout << "========================================\n\n";

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try {
        HttpServerConfig config;
        config.host = "0.0.0.0";
        config.port = port;
        config.io_scheduler_count = io_threads;
        config.compute_scheduler_count = 0;

        HttpServer server(config);

        HTTP_LOG_INFO("[server] [listen] [ws] [{}:{}]", config.host, config.port);

        server.start(handleHttpRequest);

        std::cout << "Server started successfully!\n";
        std::cout << "Waiting for requests...\n\n";

        // 等待停止信号
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "\nShutting down...\n";
        server.stop();
        std::cout << "Server stopped.\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
