/**
 * @file T25-H2cServer.cc
 * @brief H2c 服务器测试程序
 * @details 用于测试 H2c 服务器功能
 *
 * 使用方法:
 *   ./test/T25-H2cServer [port]
 *   默认端口: 9080
 */

#include "galay-http/kernel/http2/Http2Server.h"
#include "galay-http/kernel/http/HttpLog.h"
#include <iostream>
#include <atomic>
#include <csignal>

using namespace galay::http2;
using namespace galay::kernel;

static std::atomic<bool> g_running{true};
static std::atomic<uint64_t> g_request_count{0};

void signalHandler(int) {
    g_running = false;
}

/**
 * @brief 处理 HTTP/2 请求
 */
Coroutine handleRequest(Http2ConnImpl<galay::async::TcpSocket>& conn, Http2Stream::ptr stream, Http2Request request) {
    g_request_count++;

    HTTP_LOG_INFO("Request #{}: {} {} (stream {})",
                  g_request_count.load(), request.method, request.path, stream->streamId());

    // 构造响应
    Http2Response response;
    response.status = 200;
    response.headers.push_back({"content-type", "text/plain"});
    response.headers.push_back({"server", "Galay-H2c-Test/1.0"});

    std::string body = "Hello from H2c Test Server!\n";
    body += "Request #" + std::to_string(g_request_count.load()) + "\n";
    body += "Method: " + request.method + "\n";
    body += "Path: " + request.path + "\n";
    body += "Stream ID: " + std::to_string(stream->streamId()) + "\n";

    response.body = body;

    // 构建响应头部
    std::vector<Http2HeaderField> headers;
    headers.push_back({":status", std::to_string(response.status)});
    for (const auto& h : response.headers) {
        headers.push_back(h);
    }

    bool has_body = !response.body.empty();

    // 发送 HEADERS
    HTTP_LOG_DEBUG("Sending HEADERS for stream {}", stream->streamId());
    while (true) {
        auto result = co_await conn.sendHeaders(stream->streamId(), headers, !has_body, true);
        if (!result) {
            HTTP_LOG_ERROR("Failed to send headers: {}", http2ErrorCodeToString(result.error()));
            co_return;
        }
        if (result.value()) break;
    }
    HTTP_LOG_DEBUG("HEADERS sent for stream {}", stream->streamId());

    // 发送 DATA
    if (has_body) {
        HTTP_LOG_DEBUG("Sending DATA for stream {}, size: {}", stream->streamId(), response.body.size());
        while (true) {
            auto result = co_await conn.sendDataFrame(stream->streamId(), response.body, true);
            if (!result) {
                HTTP_LOG_ERROR("Failed to send data: {}", http2ErrorCodeToString(result.error()));
                co_return;
            }
            if (result.value()) break;
        }
        HTTP_LOG_DEBUG("DATA sent for stream {}", stream->streamId());
    }

    HTTP_LOG_INFO("Response sent for stream {}", stream->streamId());
    co_return;
}

int main(int argc, char* argv[]) {
    uint16_t port = 9080;
    if (argc > 1) {
        port = std::atoi(argv[1]);
    }

    std::cout << "========================================\n";
    std::cout << "H2c Server Test\n";
    std::cout << "========================================\n";
    std::cout << "Port: " << port << "\n";
    std::cout << "Test command: ./test/T25-H2cClient localhost " << port << "\n";
    std::cout << "Press Ctrl+C to stop\n";
    std::cout << "========================================\n\n";

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try {
        H2cServerConfig config;
        config.host = "0.0.0.0";
        config.port = port;
        config.io_scheduler_count = 4;
        config.compute_scheduler_count = 0;
        config.max_concurrent_streams = 100;
        config.initial_window_size = 65535;
        config.enable_push = false;

        H2cServer server(config);

        HTTP_LOG_INFO("H2c test server starting on {}:{}", config.host, config.port);

        server.start(handleRequest);

        std::cout << "Server started successfully!\n\n";

        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "\n\nShutting down...\n";
        std::cout << "Total requests handled: " << g_request_count << "\n";

        server.stop();
        std::cout << "Server stopped.\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
