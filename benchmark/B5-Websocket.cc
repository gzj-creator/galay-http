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
#include "kernel/websocket/WsWriterSetting.h"
#include "galay-kernel/concurrency/AsyncMutex.h"
#include <iostream>
#include <atomic>
#include <vector>
#include <thread>
#include <algorithm>
#include <numeric>
#include <signal.h>

using namespace galay::http;
using namespace galay::websocket;
using namespace galay::kernel;

// 统计信息
std::atomic<int> total_connections{0};
std::atomic<int> total_messages{0};
std::atomic<long long> total_bytes{0};
std::atomic<bool> g_running{true};
AsyncMutex g_conn_mutex;
std::vector<std::pair<uint64_t, uint64_t>> g_conn_stats;

void signalHandler(int) {
    g_running = false;
}

/**
 * @brief WebSocket 连接处理协程
 */
Coroutine handleWebSocketConnection(WsConn& ws_conn) {
    total_connections++;

    auto reader = ws_conn.getReader();
    auto writer = ws_conn.getWriter(WsWriterSetting::byServer());
    uint64_t conn_messages = 0;
    uint64_t conn_bytes = 0;
    HTTP_LOG_INFO("Send Hello To client");
    // 发送欢迎消息
    while (true) {
        auto res = co_await writer.sendText("Welcome to WebSocket Benchmark Server!");
        if(!res) {
            HTTP_LOG_ERROR("send Hello failed: {}", res.error().message());
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
            conn_messages++;
            conn_bytes += message.size();

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

    auto lock_result = co_await g_conn_mutex.lock();
    if (lock_result.has_value()) {
        g_conn_stats.emplace_back(conn_messages, conn_bytes);
        g_conn_mutex.unlock();
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
        WsConn ws_conn = WsConn::from(std::move(conn), true);

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

        std::thread stats_thread([] {
            uint64_t last_messages = 0;
            uint64_t last_bytes = 0;
            auto last_time = std::chrono::steady_clock::now();
            while (g_running.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                auto now = std::chrono::steady_clock::now();
                auto cur_messages = static_cast<uint64_t>(total_messages.load());
                auto cur_bytes = static_cast<uint64_t>(total_bytes.load());
                auto delta_sec = std::chrono::duration<double>(now - last_time).count();
                auto delta_messages = cur_messages - last_messages;
                auto delta_bytes = cur_bytes - last_bytes;
                if (delta_sec > 0.0) {
                    std::cout << "[Stats] QPS: " << (delta_messages / delta_sec)
                              << " | MB/s: " << (delta_bytes / 1024.0 / 1024.0 / delta_sec)
                              << std::endl;
                }
                last_time = now;
                last_messages = cur_messages;
                last_bytes = cur_bytes;
            }
        });

        // 等待停止信号
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "\nShutting down..." << std::endl;
        server.stop();
        if (stats_thread.joinable()) {
            stats_thread.join();
        }

        // 打印统计信息
        std::cout << "\n========================================" << std::endl;
        std::cout << "Benchmark Statistics:" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "Total connections: " << total_connections.load() << std::endl;
        std::cout << "Total messages: " << total_messages.load() << std::endl;
        std::cout << "Total bytes: " << total_bytes.load() << " ("
                  << (total_bytes.load() / 1024.0 / 1024.0) << " MB)" << std::endl;
        while (!g_conn_mutex.tryLock()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!g_conn_stats.empty()) {
            std::vector<uint64_t> msg_counts;
            std::vector<uint64_t> byte_counts;
            msg_counts.reserve(g_conn_stats.size());
            byte_counts.reserve(g_conn_stats.size());
            for (const auto& stat : g_conn_stats) {
                msg_counts.push_back(stat.first);
                byte_counts.push_back(stat.second);
            }
            auto minmax_msg = std::minmax_element(msg_counts.begin(), msg_counts.end());
            auto minmax_bytes = std::minmax_element(byte_counts.begin(), byte_counts.end());
            uint64_t sum_msg = std::accumulate(msg_counts.begin(), msg_counts.end(), uint64_t(0));
            uint64_t sum_bytes = std::accumulate(byte_counts.begin(), byte_counts.end(), uint64_t(0));
            double avg_msg = static_cast<double>(sum_msg) / msg_counts.size();
            double avg_bytes = static_cast<double>(sum_bytes) / byte_counts.size();
            std::cout << "\nPer-connection stats:" << std::endl;
            std::cout << "  Connections: " << g_conn_stats.size() << std::endl;
            std::cout << "  Messages: min " << *minmax_msg.first
                      << ", avg " << avg_msg
                      << ", max " << *minmax_msg.second << std::endl;
            std::cout << "  Bytes:    min " << *minmax_bytes.first
                      << ", avg " << avg_bytes
                      << ", max " << *minmax_bytes.second << std::endl;
        }
        g_conn_mutex.unlock();
        std::cout << "========================================" << std::endl;

        std::cout << "Server stopped." << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
