/**
 * @file B11-WssServer.cc
 * @brief WSS (WebSocket Secure) 服务器压测程序
 * @details 配合 B12-WssClient 进行 WSS 性能测试
 */

#include <chrono>
#include <csignal>
#include <iostream>
#include <atomic>
#include <thread>
#include "galay-http/kernel/http/HttpServer.h"
#include "galay-http/kernel/websocket/WsUpgrade.h"
#include "galay-http/protoc/websocket/WebSocketFrame.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/utils/Http1_1ResponseBuilder.h"
#include "galay-http/kernel/http/HttpLog.h"

#ifdef GALAY_HTTP_SSL_ENABLED

using namespace galay::http;
using namespace galay::websocket;
using namespace galay::kernel;
using namespace std::chrono_literals;

// 统计信息
static std::atomic<bool> g_running{true};
static std::atomic<uint64_t> g_total_connections{0};
static std::atomic<uint64_t> g_active_connections{0};
static std::atomic<uint64_t> g_total_messages{0};
static std::atomic<uint64_t> g_total_bytes{0};

void signalHandler(int) {
    g_running = false;
}

/**
 * @brief 处理 WSS 连接（使用底层帧处理）
 */
Coroutine handleWssConnection(galay::ssl::SslSocket& socket) {
    HTTP_LOG_DEBUG("[wss] [conn] [open]");
    g_total_connections.fetch_add(1, std::memory_order_relaxed);
    g_active_connections.fetch_add(1, std::memory_order_relaxed);

    // 发送欢迎消息
    WsFrame welcome_frame = WsFrameParser::createTextFrame("Welcome to WSS Benchmark Server!");
    std::string welcome_data = WsFrameParser::toBytes(welcome_frame, false);

    size_t sent = 0;
    while (sent < welcome_data.size()) {
        auto result = co_await socket.send(welcome_data.data() + sent, welcome_data.size() - sent);
        if (!result) {
            HTTP_LOG_ERROR("[wss] [welcome] [send-fail] [{}]", result.error().message());
            g_active_connections.fetch_sub(1, std::memory_order_relaxed);
            co_return;
        }
        sent += result.value();
    }

    // 接收缓冲区
    std::vector<char> buffer(8192);
    std::string accumulated;

    // 消息循环
    while (true) {
        auto recv_result = co_await socket.recv(buffer.data(), buffer.size());
        if (!recv_result) {
            HTTP_LOG_DEBUG("[wss] [recv-fail] [{}]", recv_result.error().message());
            break;
        }

        size_t bytes_received = recv_result.value().size();
        if (bytes_received == 0) {
            HTTP_LOG_DEBUG("[wss] [conn] [closed]");
            break;
        }

        accumulated.append(buffer.data(), bytes_received);

        // 尝试解析帧
        while (!accumulated.empty()) {
            WsFrame frame;
            std::vector<iovec> iovecs;
            iovecs.push_back({const_cast<char*>(accumulated.data()), accumulated.size()});

            auto parse_result = WsFrameParser::fromIOVec(iovecs, frame, true);
            if (!parse_result) {
                if (parse_result.error().code() == kWsIncomplete) {
                    break;  // 需要更多数据
                }
                HTTP_LOG_ERROR("[wss] [frame] [parse-fail] [{}]", parse_result.error().message());
                goto cleanup;
            }

            size_t consumed = parse_result.value();
            accumulated.erase(0, consumed);

            // 处理帧
            if (frame.header.opcode == WsOpcode::Close) {
                HTTP_LOG_DEBUG("[wss] [close] [recv]");
                WsFrame close_frame = WsFrameParser::createCloseFrame(WsCloseCode::Normal);
                std::string close_data = WsFrameParser::toBytes(close_frame, false);
                co_await socket.send(close_data.data(), close_data.size());
                goto cleanup;
            }
            else if (frame.header.opcode == WsOpcode::Ping) {
                HTTP_LOG_DEBUG("[wss] [ping] [recv] [pong] [send]");
                WsFrame pong_frame = WsFrameParser::createPongFrame(frame.payload);
                std::string pong_data = WsFrameParser::toBytes(pong_frame, false);
                co_await socket.send(pong_data.data(), pong_data.size());
            }
            else if (frame.header.opcode == WsOpcode::Text || frame.header.opcode == WsOpcode::Binary) {
                g_total_messages.fetch_add(1, std::memory_order_relaxed);
                g_total_bytes.fetch_add(frame.payload.size(), std::memory_order_relaxed);

                // 回显消息（不添加前缀，直接回显）
                WsFrame echo_frame = (frame.header.opcode == WsOpcode::Text)
                    ? WsFrameParser::createTextFrame(frame.payload)
                    : WsFrameParser::createBinaryFrame(frame.payload);
                std::string echo_data = WsFrameParser::toBytes(echo_frame, false);

                size_t echo_sent = 0;
                while (echo_sent < echo_data.size()) {
                    auto r = co_await socket.send(echo_data.data() + echo_sent, echo_data.size() - echo_sent);
                    if (!r) break;
                    echo_sent += r.value();
                }
            }
        }
    }

cleanup:
    co_await socket.close();
    g_active_connections.fetch_sub(1, std::memory_order_relaxed);
    HTTP_LOG_DEBUG("[wss] [conn] [closed]");
    co_return;
}

/**
 * @brief HTTPS 请求处理器（处理 WSS 升级）
 */
Coroutine httpsHandler(HttpConnImpl<galay::ssl::SslSocket> conn) {
    HTTP_LOG_DEBUG("[https] [handler] [start]");
    auto reader = conn.getReader();
    HttpRequest request;

    // 读取请求
    while (true) {
        auto r = co_await reader.getRequest(request);
        if (!r) {
            HTTP_LOG_ERROR("[https] [req] [read-fail] [{}]", r.error().message());
            co_await conn.close();
            co_return;
        }
        if (r.value()) break;
    }

    HTTP_LOG_DEBUG("[https] [req] [{}] [{}]", httpMethodToString(request.header().method()), request.header().uri());

    // 检查是否是 WebSocket 升级请求
    std::string uri = request.header().uri();
    if (uri == "/ws" || uri.starts_with("/ws?") || uri == "/") {
        auto upgrade_result = WsUpgrade::handleUpgrade(request);

        if (!upgrade_result.success) {
            HTTP_LOG_ERROR("[wss] [upgrade] [fail] [{}]", upgrade_result.error_message);
            auto writer = conn.getWriter();
            while (true) {
                auto r = co_await writer.sendResponse(upgrade_result.response);
                if (!r || r.value()) break;
            }
            co_await conn.close();
            co_return;
        }

        HTTP_LOG_DEBUG("[wss] [upgrade] [ok]");

        // 发送 101 Switching Protocols
        auto writer = conn.getWriter();
        while (true) {
            auto r = co_await writer.sendResponse(upgrade_result.response);
            if (!r) {
                HTTP_LOG_ERROR("[wss] [upgrade] [send-fail] [{}]", r.error().message());
                co_await conn.close();
                co_return;
            }
            if (r.value()) break;
        }

        // 获取底层 socket 并处理 WebSocket 连接
        auto& socket = conn.getSocket();
        co_await handleWssConnection(socket).wait();
        co_return;
    }

    // 非 WebSocket 请求，返回 404
    auto response = Http1_1ResponseBuilder()
        .status(HttpStatusCode::NotFound_404)
        .body("Not Found")
        .buildMove();

    auto writer = conn.getWriter();
    while (true) {
        auto r = co_await writer.sendResponse(response);
        if (!r || r.value()) break;
    }
    co_await conn.close();
    co_return;
}

int main(int argc, char* argv[]) {
    // 设置日志为文件模式
    galay::http::HttpLogger::file("B11-WssServer.log");

    int port = 8443;
    std::string cert_path = "../cert/test.crt";
    std::string key_path = "../cert/test.key";

    if (argc > 1) port = std::atoi(argv[1]);
    if (argc > 2) cert_path = argv[2];
    if (argc > 3) key_path = argv[3];

    std::cout << "========================================\n";
    std::cout << "WSS (WebSocket Secure) Benchmark Server\n";
    std::cout << "========================================\n";
    std::cout << "Port: " << port << "\n";
    std::cout << "Cert: " << cert_path << "\n";
    std::cout << "Key:  " << key_path << "\n";
    std::cout << "WSS endpoint: wss://localhost:" << port << "/ws\n";
    std::cout << "Press Ctrl+C to stop\n";
    std::cout << "========================================\n\n";

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try {
        HttpsServerConfig config;
        config.host = "0.0.0.0";
        config.port = port;
        config.cert_path = cert_path;
        config.key_path = key_path;
        config.io_scheduler_count = 4;

        HttpsServer server(config);

        server.start(httpsHandler);

        std::cout << "Server started successfully!\n\n";

        // 统计线程
        std::thread stats_thread([] {
            uint64_t last_messages = 0;
            uint64_t last_bytes = 0;
            auto last_time = std::chrono::steady_clock::now();

            while (g_running.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::seconds(1));

                auto now = std::chrono::steady_clock::now();
                auto cur_messages = g_total_messages.load(std::memory_order_relaxed);
                auto cur_bytes = g_total_bytes.load(std::memory_order_relaxed);
                auto delta_sec = std::chrono::duration<double>(now - last_time).count();
                auto delta_messages = cur_messages - last_messages;
                auto delta_bytes = cur_bytes - last_bytes;

                if (delta_sec > 0.0) {
                    std::cout << "[Stats] Active: " << g_active_connections.load(std::memory_order_relaxed)
                              << " | QPS: " << static_cast<uint64_t>(delta_messages / delta_sec)
                              << " | MB/s: " << (delta_bytes / 1024.0 / 1024.0 / delta_sec)
                              << "\n";
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

        std::cout << "\nShutting down...\n";
        server.stop();

        if (stats_thread.joinable()) {
            stats_thread.join();
        }

        // 打印统计信息
        std::cout << "\n========================================\n";
        std::cout << "Benchmark Statistics:\n";
        std::cout << "========================================\n";
        std::cout << "Total connections: " << g_total_connections.load() << "\n";
        std::cout << "Active connections: " << g_active_connections.load() << "\n";
        std::cout << "Total messages: " << g_total_messages.load() << "\n";
        std::cout << "Total bytes: " << g_total_bytes.load() << " ("
                  << (g_total_bytes.load() / 1024.0 / 1024.0) << " MB)\n";
        std::cout << "========================================\n";

        std::cout << "Server stopped.\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

#else

int main() {
    std::cout << "SSL support is not enabled.\n";
    std::cout << "Rebuild with -DGALAY_HTTP_ENABLE_SSL=ON\n";
    return 0;
}

#endif
