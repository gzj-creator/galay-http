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
 * @brief 处理单个流的请求
 */
Coroutine handleStream(Http2Stream::ptr stream) {
    g_request_count++;

    // 读取完整请求
    co_await stream->readRequest().wait();
    auto& req = stream->request();

    if (req.method.empty()) co_return;

    HTTP_LOG_INFO("Request #{}: {} {} (stream {})",
                  g_request_count.load(), req.method, req.path, stream->streamId());

    // 构造响应
    std::string resp_body = "Hello from H2c Test Server!\n";
    resp_body += "Request #" + std::to_string(g_request_count.load()) + "\n";
    resp_body += "Method: " + req.method + "\n";
    resp_body += "Path: " + req.path + "\n";
    resp_body += "Stream ID: " + std::to_string(stream->streamId()) + "\n";

    co_await stream->replyAndWait(
        Http2Headers().status(200).contentType("text/plain").server("Galay-H2c-Test/1.0"),
        resp_body).wait();

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

        server.start(handleStream);

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
