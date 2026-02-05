/**
 * @file B3-H2cServer.cc
 * @brief HTTP/2 Cleartext (h2c) 服务器压测程序（纯净版）
 * @details 高性能 H2c 服务器，移除统计功能，由客户端负责统计
 */

#include "galay-http/kernel/http2/Http2Server.h"
#include "galay-http/kernel/http/HttpLog.h"
#include <iostream>
#include <csignal>

using namespace galay::http2;
using namespace galay::kernel;

static volatile bool g_running = true;

void signalHandler(int) {
    g_running = false;
}

/**
 * @brief 处理 HTTP/2 请求
 */
Coroutine handleRequest(Http2ConnImpl<TcpSocket>& conn, Http2Stream::ptr stream, Http2Request request) {
    // 构造响应
    Http2Response response;
    response.status = 200;
    response.headers.push_back({"content-type", "text/plain"});

    std::string body = "Hello from H2c Server!";
    response.headers.push_back({"content-length", std::to_string(body.size())});
    response.body = body;

    // 发送响应
    std::vector<Http2HeaderField> headers;
    headers.push_back({":status", std::to_string(response.status)});
    for (const auto& h : response.headers) {
        headers.push_back(h);
    }

    // 发送 HEADERS 帧
    while (true) {
        auto result = co_await conn.sendHeaders(stream->streamId(), headers, false, true);
        if (!result) {
            HTTP_LOG_ERROR("[h2c] [headers] [send-fail]");
            co_return;
        }
        if (result.value()) break;
    }

    // 发送 DATA 帧
    while (true) {
        auto result = co_await conn.sendDataFrame(stream->streamId(), response.body, true);
        if (!result) {
            HTTP_LOG_ERROR("[h2c] [data] [send-fail]");
            co_return;
        }
        if (result.value()) break;
    }

    co_return;
}

int main(int argc, char* argv[]) {
    // 禁用日志以获得最佳性能
    galay::http::HttpLogger::disable();

    uint16_t port = 9080;
    int io_threads = 4;

    if (argc > 1) {
        port = std::atoi(argv[1]);
    }
    if (argc > 2) {
        io_threads = std::atoi(argv[2]);
    }

    std::cout << "========================================\n";
    std::cout << "HTTP/2 Cleartext (h2c) Server Benchmark\n";
    std::cout << "========================================\n";
    std::cout << "Port: " << port << "\n";
    std::cout << "IO Threads: " << io_threads << "\n";
    std::cout << "Test command: ./build/benchmark/B4-H2cClient localhost " << port << " <connections> <requests>\n";
    std::cout << "Press Ctrl+C to stop\n";
    std::cout << "========================================\n\n";

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try {
        H2cServerConfig config;
        config.host = "0.0.0.0";
        config.port = port;
        config.io_scheduler_count = io_threads;
        config.compute_scheduler_count = 0;
        config.max_concurrent_streams = 1000;
        config.initial_window_size = 65535;

        H2cServer server(config);

        server.start(handleRequest);

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
