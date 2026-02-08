/**
 * @file E14-H2cMuxServer.cc
 * @brief h2c 多路复用服务器示例
 * @details 演示同一连接上多个 stream 并发处理（通过延迟制造乱序返回）
 *
 * 测试方法:
 *   ./E14-H2cMuxServer 8080
 */

#include "galay-http/kernel/http2/Http2Server.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

using namespace galay::http2;
using namespace galay::kernel;

static std::atomic<bool> g_running{true};
static std::atomic<uint64_t> g_requests{0};

void signalHandler(int) {
    g_running = false;
}

static int parseDelayMs(const std::string& path) {
    const std::string prefix = "/delay/";
    if (path.rfind(prefix, 0) != 0) {
        return -1;
    }
    std::string value = path.substr(prefix.size());
    if (value.empty()) return -1;
    try {
        return std::stoi(value);
    } catch (...) {
        return -1;
    }
}

Coroutine handleStream(Http2Stream::ptr stream) {
    g_requests++;

    // 读取完整请求
    co_await stream->readRequest().wait();
    auto& req = stream->request();

    HTTP_LOG_INFO("[h2c] [req] [{}] [{}] [stream={}]", req.method, req.path, stream->streamId());

    int delay_ms = parseDelayMs(req.path);
    if (delay_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }

    std::string body = "stream=" + std::to_string(stream->streamId()) + " path=" + req.path;
    if (delay_ms > 0) {
        body += " delay=" + std::to_string(delay_ms) + "ms";
    }

    co_await stream->replyAndWait(
        Http2Headers().status(200).contentType("text/plain").server("Galay-H2cMux/1.0"),
        body).wait();

    co_return;
}

int main(int argc, char* argv[]) {
    int port = 8080;
    if (argc > 1) {
        port = std::atoi(argv[1]);
    }

    std::cout << "========================================\n";
    std::cout << "H2c Mux Server Example\n";
    std::cout << "========================================\n";

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try {
        H2cServerConfig config;
        config.host = "0.0.0.0";
        config.port = port;
        config.io_scheduler_count = 4;
        config.max_concurrent_streams = 100;
        config.enable_push = false;

        H2cServer server(config);

        std::cout << "Server running on http://0.0.0.0:" << port << "\n";
        std::cout << "Test: ./E15-H2cMuxClient localhost " << port << "\n";
        std::cout << "Press Ctrl+C to stop\n";
        std::cout << "========================================\n";

        server.start(handleStream);

        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "\nTotal requests: " << g_requests << "\n";
        server.stop();
        std::cout << "Server stopped.\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
