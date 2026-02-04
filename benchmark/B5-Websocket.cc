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
    int conn_id = total_connections.fetch_add(1);

    auto reader = ws_conn.getReader();
    auto writer = ws_conn.getWriter(WsWriterSetting::byServer());
    uint64_t conn_messages = 0;
    uint64_t conn_bytes = 0;
    HTTP_LOG_INFO("[ws] [conn-{}] [start]", conn_id);

    // 发送欢迎消息
    HTTP_LOG_INFO("[ws] [conn-{}] [welcome] [sending]", conn_id);
    while (true) {
        auto res = co_await writer.sendText("Welcome to WebSocket Benchmark Server!");
        if(!res) {
            HTTP_LOG_ERROR("[ws] [conn-{}] [welcome] [send-fail] [{}]", conn_id, res.error().message());
            co_return;
        }
        if(res.value()) {
            HTTP_LOG_INFO("[ws] [conn-{}] [welcome] [sent]", conn_id);
            break;
        }
    }

    // 消息循环
    while (true) {
        std::string message;
        WsOpcode opcode;

        HTTP_LOG_INFO("[ws] [conn-{}] [waiting-message]", conn_id);
        auto result = co_await reader.getMessage(message, opcode);

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
            total_messages++;
            total_bytes += message.size();
            conn_messages++;
            conn_bytes += message.size();

            HTTP_LOG_INFO("[ws] [conn-{}] [recv] [opcode={}] [size={}] [total-msg={}]",
                         conn_id, static_cast<int>(opcode), message.size(), conn_messages);

            // 回显消息
            if (opcode == WsOpcode::Text) {
                HTTP_LOG_INFO("[ws] [conn-{}] [echo-text] [sending]", conn_id);
                while (true) {
                    auto send_res = co_await writer.sendText(message);
                    if (!send_res) {
                        HTTP_LOG_ERROR("[ws] [conn-{}] [echo-text] [send-fail] [{}]", conn_id, send_res.error().message());
                        goto cleanup;
                    }
                    if (send_res.value()) {
                        HTTP_LOG_INFO("[ws] [conn-{}] [echo-text] [sent]", conn_id);
                        break;
                    }
                }
            } else {
                HTTP_LOG_INFO("[ws] [conn-{}] [echo-binary] [sending]", conn_id);
                while (true) {
                    auto send_res = co_await writer.sendBinary(message);
                    if (!send_res) {
                        HTTP_LOG_ERROR("[ws] [conn-{}] [echo-binary] [send-fail] [{}]", conn_id, send_res.error().message());
                        goto cleanup;
                    }
                    if (send_res.value()) {
                        HTTP_LOG_INFO("[ws] [conn-{}] [echo-binary] [sent]", conn_id);
                        break;
                    }
                }
            }

        } else if (opcode == WsOpcode::Ping) {
            // 响应 Ping
            HTTP_LOG_INFO("[ws] [conn-{}] [ping] [responding]", conn_id);
            while (true) {
                auto pong_res = co_await writer.sendPong(message);
                if (!pong_res) {
                    HTTP_LOG_ERROR("[ws] [conn-{}] [pong] [send-fail] [{}]", conn_id, pong_res.error().message());
                    goto cleanup;
                }
                if (pong_res.value()) {
                    HTTP_LOG_INFO("[ws] [conn-{}] [pong] [sent]", conn_id);
                    break;
                }
            }

        } else if (opcode == WsOpcode::Close) {
            // 客户端关闭连接
            HTTP_LOG_INFO("[ws] [conn-{}] [close-requested]", conn_id);
            while (true) {
                auto close_res = co_await writer.sendClose();
                if (!close_res) {
                    HTTP_LOG_ERROR("[ws] [conn-{}] [close] [send-fail] [{}]", conn_id, close_res.error().message());
                    break;
                }
                if (close_res.value()) {
                    HTTP_LOG_INFO("[ws] [conn-{}] [close] [sent]", conn_id);
                    break;
                }
            }
            break;
        }
    }

cleanup:
    HTTP_LOG_INFO("[ws] [conn-{}] [cleanup] [messages={}] [bytes={}]", conn_id, conn_messages, conn_bytes);

    auto lock_result = co_await g_conn_mutex.lock();
    g_conn_stats.emplace_back(conn_messages, conn_bytes);
    g_conn_mutex.unlock();
    co_await ws_conn.close();
    co_return;
}

/**
 * @brief HTTP 请求处理协程
 */
Coroutine handleHttpRequest(HttpConn conn) {
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
            co_await writer.sendResponse(upgrade_result.response);
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
        co_await handleWebSocketConnection(ws_conn).wait();
        HTTP_LOG_INFO("[http] [req-{}] [ws-handler] [done]", current_req_id);
    } else {
        // 非 WebSocket 请求，返回 404
        HTTP_LOG_INFO("[http] [req-{}] [not-ws] [404]", current_req_id);
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
    // 设置日志为文件模式
    galay::http::HttpLogger::file("B5-Websocket.log");

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

        HTTP_LOG_INFO("[server] [listen] [ws] [{}:{}]", config.host, config.port);

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
