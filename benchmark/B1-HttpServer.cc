/**
 * @file B1-HttpServer.cc
 * @brief HTTP 服务器压测程序（纯净版）
 * @details 启动一个高性能 HTTP 服务器用于压测
 *          移除所有统计功能，由客户端负责统计
 *
 * 使用方法:
 *   ./benchmark/B1-HttpServer [port] [io_threads]
 *   默认端口: 8080
 *   默认线程数: 4
 *
 * 压测命令:
 *   wrk -t4 -c100 -d30s --latency http://127.0.0.1:8080/
 *   wrk -t8 -c500 -d30s --latency http://127.0.0.1:8080/
 */

#include "galay-http/kernel/http/HttpServer.h"
#include "galay-http/kernel/http/HttpConn.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/utils/Http1_1ResponseBuilder.h"
#include "galay-http/kernel/http/HttpLog.h"
#include <iostream>
#include <csignal>
using namespace galay::http;
using namespace galay::kernel;

static volatile bool g_running = true;

void signalHandler(int) {
    g_running = false;
}

/**
 * @brief HTTP 请求处理器 - 简单的 OK 响应
 */
Task<void> handleHttpRequest(HttpConn conn) {
    while(true) {
        auto reader = conn.getReader();
        HttpRequest request;

        while (true) {
            auto read_result = co_await reader.getRequest(request);
            if (!read_result) {
                co_return;
            }
            if (read_result.value()) break;
        }

        // 构建响应
        auto response = Http1_1ResponseBuilder()
            .status(HttpStatusCode::OK_200)
            .header("Content-Type", "text/plain")
            .header("Connection", "keep-alive")
            .body("OK")
            .buildMove();

        auto writer = conn.getWriter();
        auto result = co_await writer.sendResponse(response);
        if (!result) {
            HTTP_LOG_ERROR("[send] [fail] [{}]", result.error().message());
        }
    }

    co_return;
}

int main(int argc, char* argv[]) {
    // 禁用日志以获得最佳性能
    galay::http::HttpLogger::disable();

    uint16_t port = 8080;
    int io_threads = 4;

    if (argc > 1) {
        port = std::atoi(argv[1]);
    }
    if (argc > 2) {
        io_threads = std::atoi(argv[2]);
    }

    std::cout << "========================================\n";
    std::cout << "HTTP Server Benchmark\n";
    std::cout << "========================================\n";
    std::cout << "Port: " << port << "\n";
    std::cout << "IO Threads: " << io_threads << "\n";
    std::cout << "Endpoint: http://127.0.0.1:" << port << "/\n";
    std::cout << "\nBenchmark commands:\n";
    std::cout << "  wrk -t4 -c100 -d30s --latency http://127.0.0.1:" << port << "/\n";
    std::cout << "  wrk -t8 -c500 -d30s --latency http://127.0.0.1:" << port << "/\n";
    std::cout << "\nPress Ctrl+C to stop\n";
    std::cout << "========================================\n\n";

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try {
        HttpServer server(HttpServerBuilder()
            .host("0.0.0.0")
            .port(port)
            .ioSchedulerCount(static_cast<size_t>(io_threads))
            .computeSchedulerCount(0)
            .build());

        HTTP_LOG_INFO("[server] [listen] [http] [{}:{}]", "0.0.0.0", port);

        server.start(handleHttpRequest);

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
