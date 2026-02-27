/**
 * @file B10-H2cMuxServer.cc
 * @brief HTTP/2 多路复用 Echo 服务器压测程序
 * @details 与 B3 相同的 echo 逻辑，但配置更高的 max_concurrent_streams
 *          以支持单连接上大量并发流的压测场景
 *
 * 使用方法:
 *   ./B10-H2cMuxServer [port] [io_threads] [max_streams] [debug]
 *   默认: 9080 4 1000 0
 *
 * 压测命令:
 *   ./B11-H2cMuxClient localhost 9080 10 100 50
 */

#include "galay-http/kernel/http2/Http2Server.h"
#include "galay-http/kernel/http/HttpLog.h"
#include <iostream>
#include <csignal>
#include <atomic>

using namespace galay::http2;
using namespace galay::kernel;

static volatile bool g_running = true;

void signalHandler(int) {
    g_running = false;
}

Coroutine handleStream(Http2Stream::ptr stream) {
    std::string body;

    while (true) {
        auto frame_result = co_await stream->getFrame();
        if (!frame_result) break;
        auto frame = std::move(frame_result.value());
        if (!frame) break;
        if (frame->isData()) {
            body.append(frame->asData()->data());
        }
        if (frame->isEndStream()) break;
    }

    co_await stream->replyAndWait(
        Http2Headers().status(200).contentType("text/plain").contentLength(body.size()),
        body).wait();

    co_return;
}

int main(int argc, char* argv[]) {
    uint16_t port = 9080;
    int io_threads = 4;
    uint32_t max_streams = 1000;
    int debug_log = 0;

    if (argc > 1) port = std::atoi(argv[1]);
    if (argc > 2) io_threads = std::atoi(argv[2]);
    if (argc > 3) max_streams = std::atoi(argv[3]);
    if (argc > 4) debug_log = std::atoi(argv[4]);

    if (debug_log > 0) {
        galay::http::HttpLogger::console();
    } else {
        galay::http::HttpLogger::disable();
    }

    std::cout << "========================================\n";
    std::cout << "H2c Mux Server Benchmark\n";
    std::cout << "========================================\n";
    std::cout << "Port: " << port << "\n";
    std::cout << "IO Threads: " << io_threads << "\n";
    std::cout << "Max Concurrent Streams: " << max_streams << "\n";
    std::cout << "Debug Log: " << (debug_log > 0 ? "ON" : "OFF") << "\n";
    std::cout << "Press Ctrl+C to stop\n";
    std::cout << "========================================\n\n";

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try {
        H2cServer server(H2cServerBuilder()
            .host("0.0.0.0")
            .port(port)
            .ioSchedulerCount(static_cast<size_t>(io_threads))
            .computeSchedulerCount(0)
            .maxConcurrentStreams(static_cast<uint32_t>(max_streams))
            .initialWindowSize(65535)
            .build());
        server.start(handleStream);

        std::cout << "Server started. Waiting for requests...\n\n";

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
