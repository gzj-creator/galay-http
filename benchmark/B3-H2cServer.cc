/**
 * @file B3-H2cServer.cc
 * @brief HTTP/2 Cleartext (h2c) Echo 服务器压测程序
 * @details 高性能 H2c Echo 服务器，移除统计功能，由客户端负责统计
 */

#include "galay-http/kernel/http2/Http2Server.h"
#include "galay-http/kernel/http/HttpLog.h"
#include <iostream>
#include <csignal>
#include <atomic>

using namespace galay::http2;
using namespace galay::kernel;

static volatile bool g_running = true;
static bool g_debug_log = false;
static std::atomic<int> g_debug_logs{0};

void signalHandler(int) {
    g_running = false;
}

Coroutine handleStream(Http2Stream::ptr stream) {
    std::string body;

    // 消费所有帧直到 END_STREAM
    while (true) {
        auto frame_result = co_await stream->getFrame();
        if (!frame_result) break;
        auto frame = std::move(frame_result.value());
        if (!frame) break;
        if (frame->isData()) {
            auto* data = frame->asData();
            body.append(data->data());
        }
        if (frame->isEndStream()) break;
    }

    if (g_debug_log) {
        int idx = g_debug_logs.fetch_add(1);
        if (idx < 10) {
            std::cerr << "[echo] recv body_len=" << body.size() << "\n";
        }
    }

    // 构造响应（echo body）
    co_await stream->replyAndWait(
        Http2Headers().status(200).contentType("text/plain").contentLength(body.size()),
        body).wait();

    co_return;
}

int main(int argc, char* argv[]) {
    uint16_t port = 9080;
    int io_threads = 4;
    int debug_log = 0;

    if (argc > 1) {
        port = std::atoi(argv[1]);
    }
    if (argc > 2) {
        io_threads = std::atoi(argv[2]);
    }
    if (argc > 3) {
        debug_log = std::atoi(argv[3]);
    }

    // 默认禁用日志以获得最佳性能，debug_log=1 时开启
    if (debug_log > 0) {
        galay::http::HttpLogger::console();
    } else {
        galay::http::HttpLogger::disable();
    }
    g_debug_log = (debug_log > 0);

    std::cout << "========================================\n";
    std::cout << "HTTP/2 Cleartext (h2c) Server Benchmark\n";
    std::cout << "========================================\n";
    std::cout << "Port: " << port << "\n";
    std::cout << "IO Threads: " << io_threads << "\n";
    std::cout << "Debug Log: " << (debug_log > 0 ? "ON" : "OFF") << "\n";
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

        server.start(handleStream);

        std::cout << "Server started successfully!\n";
        std::cout << "Waiting for requests...\n\n";

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
