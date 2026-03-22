/**
 * @file B7-WssServer.cc
 * @brief WSS (WebSocket Secure) 服务器压测程序（纯净版）
 * @details 配合 B8-WssClient 进行 WSS 性能测试
 *          移除统计功能，由客户端负责统计
 */

#include <chrono>
#include <array>
#include <csignal>
#include <iostream>
#include "galay-http/kernel/http/HttpServer.h"
#include "galay-http/kernel/websocket/WsUpgrade.h"
#include "galay-http/protoc/websocket/WebSocketFrame.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/utils/Http1_1ResponseBuilder.h"
#include "galay-http/kernel/http/HttpLog.h"

#ifdef GALAY_HTTP_SSL_ENABLED

using namespace galay::http;
using namespace galay::websocket;
using namespace galay::kernel;

static volatile bool g_running = true;

void signalHandler(int) {
    g_running = false;
}

namespace {

void compactConsumedPrefix(std::string& buffer, size_t& offset) {
    if (offset == 0) {
        return;
    }
    if (offset >= buffer.size()) {
        buffer.clear();
        offset = 0;
        return;
    }
    if (offset >= 4096 || offset * 2 >= buffer.size()) {
        buffer.erase(0, offset);
        offset = 0;
    }
}

void encodeServerFrame(std::string& out, WsFrame& frame) {
    frame.header.mask = false;
    frame.header.payload_length = frame.payload.size();
    WsFrameParser::encodeInto(out, frame, false);
}

} // namespace

/**
 * @brief 处理 WSS 连接（使用底层帧处理）
 */
Task<void> handleWssConnection(galay::ssl::SslSocket& socket) {
    HTTP_LOG_DEBUG("[wss] [conn] [open]");

    std::string send_buffer;
    send_buffer.reserve(2048);

    WsFrame welcome_frame = WsFrameParser::createTextFrame("Welcome to WSS Benchmark Server!");
    encodeServerFrame(send_buffer, welcome_frame);

    auto result = co_await socket.send(send_buffer.data(), send_buffer.size());
    if (!result) {
        HTTP_LOG_ERROR("[wss] [welcome] [send-fail] [{}]", result.error().message());
        co_return;
    }

    std::array<char, 8192> buffer{};
    std::string accumulated;
    accumulated.reserve(buffer.size() * 2);
    size_t parse_offset = 0;

    // 消息循环
    while (true) {
        compactConsumedPrefix(accumulated, parse_offset);

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
        while (parse_offset < accumulated.size()) {
            WsFrame frame;
            std::array<iovec, 1> iovecs{{
                {
                    .iov_base = const_cast<char*>(accumulated.data() + parse_offset),
                    .iov_len = accumulated.size() - parse_offset,
                }
            }};

            auto parse_result = WsFrameParser::fromIOVec(iovecs.data(), iovecs.size(), frame, true);
            if (!parse_result) {
                if (parse_result.error().code() == kWsIncomplete) {
                    break;  // 需要更多数据
                }
                HTTP_LOG_ERROR("[wss] [frame] [parse-fail] [{}]", parse_result.error().message());
                goto cleanup;
            }

            size_t consumed = parse_result.value();
            parse_offset += consumed;

            // 处理帧
            if (frame.header.opcode == WsOpcode::Close) {
                HTTP_LOG_DEBUG("[wss] [close] [recv]");
                WsFrame close_frame = WsFrameParser::createCloseFrame(WsCloseCode::Normal);
                encodeServerFrame(send_buffer, close_frame);
                auto result = co_await socket.send(send_buffer.data(), send_buffer.size());
                if (!result) {
                    HTTP_LOG_WARN("[close] [fail] [{}]", result.error().message());
                }
                goto cleanup;
            }
            else if (frame.header.opcode == WsOpcode::Ping) {
                HTTP_LOG_DEBUG("[wss] [ping] [recv] [pong] [send]");
                frame.header.opcode = WsOpcode::Pong;
                encodeServerFrame(send_buffer, frame);
                auto result = co_await socket.send(send_buffer.data(), send_buffer.size());
                if (!result) {
                    HTTP_LOG_ERROR("[send] [fail] [{}]", result.error().message());
                }
            }
            else if (frame.header.opcode == WsOpcode::Text || frame.header.opcode == WsOpcode::Binary) {
                encodeServerFrame(send_buffer, frame);
                auto r = co_await socket.send(send_buffer.data(), send_buffer.size());
                if (!r) break;
            }

            compactConsumedPrefix(accumulated, parse_offset);
        }
    }

cleanup:
    co_await socket.close();
    HTTP_LOG_DEBUG("[wss] [conn] [closed]");
    co_return;
}

/**
 * @brief HTTPS 请求处理器（处理 WSS 升级）
 */
Task<void> httpsHandler(HttpConnImpl<galay::ssl::SslSocket> conn) {
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
            auto result = co_await writer.sendResponse(upgrade_result.response);
            if (!result) {
                HTTP_LOG_ERROR("[send] [fail] [{}]", result.error().message());
            }
            co_await conn.close();
            co_return;
        }

        HTTP_LOG_DEBUG("[wss] [upgrade] [ok]");

        // 发送 101 Switching Protocols
        auto writer = conn.getWriter();
        auto r = co_await writer.sendResponse(upgrade_result.response);
        if (!r) {
            HTTP_LOG_ERROR("[wss] [upgrade] [send-fail] [{}]", r.error().message());
            co_await conn.close();
            co_return;
        }

        // 获取底层 socket 并处理 WebSocket 连接
        auto& socket = conn.getSocket();
        co_await handleWssConnection(socket);
        co_return;
    }

    // 非 WebSocket 请求，返回 404
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
    co_return;
}

int main(int argc, char* argv[]) {
    // 禁用日志以获得最佳性能
    galay::http::HttpLogger::disable();

    int port = 8443;
    int io_threads = 4;
    std::string cert_path = "../cert/test.crt";
    std::string key_path = "../cert/test.key";

    if (argc > 1) port = std::atoi(argv[1]);
    if (argc > 2) io_threads = std::atoi(argv[2]);
    if (argc > 3) cert_path = argv[3];
    if (argc > 4) key_path = argv[4];

    std::cout << "========================================\n";
    std::cout << "WSS (WebSocket Secure) Benchmark Server\n";
    std::cout << "========================================\n";
    std::cout << "Port: " << port << "\n";
    std::cout << "IO Threads: " << io_threads << "\n";
    std::cout << "Configured Compute Threads: 0\n";
    std::cout << "Cert: " << cert_path << "\n";
    std::cout << "Key:  " << key_path << "\n";
    std::cout << "WSS endpoint: wss://localhost:" << port << "/ws\n";
    std::cout << "Press Ctrl+C to stop\n";
    std::cout << "========================================\n\n";

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try {
        HttpsServer server(HttpsServerBuilder()
            .host("0.0.0.0")
            .port(port)
            .certPath(cert_path)
            .keyPath(key_path)
            .ioSchedulerCount(static_cast<size_t>(io_threads))
            .computeSchedulerCount(0)
            .build());

        server.start(httpsHandler);

        std::cout << "Server started successfully!\n";
        std::cout << "Runtime Config: io=" << server.getRuntime().getIOSchedulerCount()
                  << " compute=" << server.getRuntime().getComputeSchedulerCount()
                  << " (configured io=" << io_threads << " compute=0)\n";
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

#else

int main() {
    std::cout << "SSL support is not enabled.\n";
    std::cout << "Rebuild with -DGALAY_HTTP_ENABLE_SSL=ON\n";
    return 0;
}

#endif
