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
#include "galay-http/utils/Http1_1ResponseBuilder.h"
#include "galay-http/kernel/http/HttpLog.h"
#include "kernel/websocket/WsWriterSetting.h"
#include <iostream>
#include <atomic>
#include <signal.h>
#include <cstdlib>
#include <string_view>
#include <utility>
#include <thread>
#include <chrono>
using namespace galay::http;
using namespace galay::websocket;
using namespace galay::kernel;

// 统计信息
std::atomic<int> total_connections{0};
std::atomic<bool> g_running{true};

void signalHandler(int) {
    g_running = false;
}

/**
 * @brief WebSocket 连接处理协程
 */
Task<void> handleWebSocketConnection(WsConn& ws_conn) {
    int conn_id = total_connections.fetch_add(1);

    auto reader = ws_conn.getReader();
    auto writer = ws_conn.getWriter(WsWriterSetting::byServer());
    HTTP_LOG_INFO("[ws] [conn-{}] [start]", conn_id);

    // 发送欢迎消息
    HTTP_LOG_INFO("[ws] [conn-{}] [welcome] [sending]", conn_id);
    auto res = co_await writer.sendText("Welcome to WebSocket Benchmark Server!");
    if(!res) {
        HTTP_LOG_ERROR("[ws] [conn-{}] [welcome] [send-fail] [{}]", conn_id, res.error().message());
        co_return;
    }
    HTTP_LOG_INFO("[ws] [conn-{}] [welcome] [sent]", conn_id);

    // 消息循环
    std::string message;
    WsOpcode opcode = WsOpcode::Text;
    while (true) {
        message.clear();

        HTTP_LOG_INFO("[ws] [conn-{}] [waiting-message]", conn_id);
        auto result = co_await ws_conn.echoOnce(message, opcode);

        if (!result) {
            // 连接错误
            HTTP_LOG_ERROR("[ws] [conn-{}] [read-error] [{}]", conn_id, result.error().message());
            break;
        }

        if (!result.value()) {
            // 消息未完成，继续读取
            HTTP_LOG_INFO("[ws] [conn-{}] [message-incomplete]", conn_id);
            continue;
        }

        // 处理不同类型的消息
        if (opcode == WsOpcode::Text || opcode == WsOpcode::Binary) {
            HTTP_LOG_INFO("[ws] [conn-{}] [recv] [opcode={}] [size={}]",
                         conn_id, static_cast<int>(opcode), message.size());
            HTTP_LOG_INFO("[ws] [conn-{}] [echo-inline] [done]", conn_id);

        } else if (opcode == WsOpcode::Ping) {
            // 响应 Ping
            HTTP_LOG_INFO("[ws] [conn-{}] [ping] [responding]", conn_id);
            auto pong_res = co_await writer.sendPong(message);
            if (!pong_res) {
                HTTP_LOG_ERROR("[ws] [conn-{}] [pong] [send-fail] [{}]", conn_id, pong_res.error().message());
                goto cleanup;
            }
            HTTP_LOG_INFO("[ws] [conn-{}] [pong] [sent]", conn_id);

        } else if (opcode == WsOpcode::Close) {
            // 客户端关闭连接
            HTTP_LOG_INFO("[ws] [conn-{}] [close-requested]", conn_id);
            auto close_res = co_await writer.sendClose();
            if (!close_res) {
                HTTP_LOG_ERROR("[ws] [conn-{}] [close] [send-fail] [{}]", conn_id, close_res.error().message());
            } else {
                HTTP_LOG_INFO("[ws] [conn-{}] [close] [sent]", conn_id);
            }
            break;
        }
    }

cleanup:
    HTTP_LOG_INFO("[ws] [conn-{}] [cleanup]", conn_id);
    co_await ws_conn.close();
    co_return;
}

/**
 * @brief HTTP 请求处理协程
 */
Task<void> handleHttpRequest(HttpConn conn) {
    static std::atomic<int> req_id{0};
    int current_req_id = req_id.fetch_add(1);

    HTTP_LOG_INFO("[http] [req-{}] [start]", current_req_id);

    auto reader = conn.getReader();
    HttpRequest request;

    HTTP_LOG_INFO("[http] [req-{}] [reading-request]", current_req_id);
    auto read_result = co_await reader.getRequest(request);
    if (!read_result) {
        HTTP_LOG_ERROR("[http] [req-{}] [read-fail] [{}]", current_req_id, read_result.error().message());
        co_await conn.close();
        co_return;
    }

    HTTP_LOG_INFO("[http] [req-{}] [read-ok] [uri={}]", current_req_id, request.header().uri());

    // 检查是否是 WebSocket 升级请求
    if (request.header().uri() == "/ws" || request.header().uri() == "/") {
        HTTP_LOG_INFO("[http] [req-{}] [ws-upgrade] [handling]", current_req_id);
        auto upgrade_result = WsUpgrade::handleUpgrade(request);

        if (!upgrade_result.success) {
            HTTP_LOG_ERROR("[http] [req-{}] [ws-upgrade] [fail]", current_req_id);
            auto writer = conn.getWriter();
            auto result = co_await writer.sendResponse(upgrade_result.response);
            if (!result) {
                HTTP_LOG_ERROR("[send] [fail] [{}]", result.error().message());
            }
            co_await conn.close();
            co_return;
        }

        HTTP_LOG_INFO("[http] [req-{}] [ws-upgrade] [sending-response]", current_req_id);
        // 发送升级响应
        auto writer = conn.getWriter();
        auto send_result = co_await writer.sendResponse(upgrade_result.response);

        if (!send_result) {
            HTTP_LOG_ERROR("[http] [req-{}] [ws-upgrade] [send-fail] [{}]", current_req_id, send_result.error().message());
            co_await conn.close();
            co_return;
        }

        HTTP_LOG_INFO("[http] [req-{}] [ws-upgrade] [response-sent] [converting-to-ws]", current_req_id);
        WsConn ws_conn = WsConn::from(std::move(conn), true);

        HTTP_LOG_INFO("[http] [req-{}] [ws-upgrade] [entering-ws-handler]", current_req_id);
        co_await handleWebSocketConnection(ws_conn);
        HTTP_LOG_INFO("[http] [req-{}] [ws-handler] [done]", current_req_id);
    } else {
        // 非 WebSocket 请求，返回 404
        HTTP_LOG_INFO("[http] [req-{}] [not-ws] [404]", current_req_id);
        auto response = Http1_1ResponseBuilder()
            .status(HttpStatusCode::NotFound_404)
            .body("Not Found")
            .buildMove();

        auto writer = conn.getWriter();
        auto result = co_await writer.sendResponse(response);
        if (!result) {
            HTTP_LOG_ERROR("[send] [fail] [{}]", result.error().message());
        }
        co_await conn.close();
    }

    co_return;
}

int main(int argc, char* argv[]) {
    // 压测默认关闭日志，避免日志 IO 成为吞吐瓶颈。
    // 设置 GALAY_HTTP_BENCH_LOG=1 可开启文件日志。
    const char* bench_log = std::getenv("GALAY_HTTP_BENCH_LOG");
    if (bench_log != nullptr && std::string_view(bench_log) == "1") {
        galay::http::HttpLogger::file("B5-Websocket.log");
    } else {
        galay::http::HttpLogger::disable();
    }

    uint16_t port = 8080;
    int io_threads = 4;
    if (argc >= 2) {
        port = std::atoi(argv[1]);
    }
    if (argc >= 3) {
        io_threads = std::atoi(argv[2]);
    }

    std::cout << "========================================" << std::endl;
    std::cout << "WebSocket Benchmark Server" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Port: " << port << std::endl;
    std::cout << "IO Threads: " << io_threads << std::endl;
    std::cout << "Configured Compute Threads: 0" << std::endl;
    std::cout << "WebSocket endpoint: ws://localhost:" << port << "/ws" << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // 设置信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try {
        HttpServer server(HttpServerBuilder()
            .host("0.0.0.0")
            .port(port)
            .ioSchedulerCount(static_cast<size_t>(io_threads))
            .computeSchedulerCount(0)
            .build());

        HTTP_LOG_INFO("[server] [listen] [ws] [{}:{}]", "0.0.0.0", port);

        server.start(handleHttpRequest);

        std::cout << "Server started successfully!\n" << std::endl;
        std::cout << "Runtime Config: io=" << server.getRuntime().getIOSchedulerCount()
                  << " compute=" << server.getRuntime().getComputeSchedulerCount()
                  << " (configured io=" << io_threads << " compute=0)" << std::endl;

        // 等待停止信号
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "\nShutting down..." << std::endl;
        server.stop();
        std::cout << "Server stopped." << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
