/**
 * @file B9-HttpServer.cc
 * @brief HTTP 服务器压测程序
 * @details 启动一个高性能 HTTP 服务器用于压测
 *
 * 使用方法:
 *   ./benchmark/B9-HttpServer [port]
 *   默认端口: 8080
 *
 * 压测命令:
 *   wrk -t4 -c100 -d30s --latency http://127.0.0.1:8080/
 *   wrk -t8 -c500 -d30s --latency http://127.0.0.1:8080/
 */

#include "galay-http/kernel/http/HttpServer.h"
#include "galay-http/kernel/http/HttpConn.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/utils/Http1_1ResponseBuilder.h"
#include "galay-http/kernel/http/HttpLog.h"
#include <iostream>
#include <atomic>
#include <csignal>
#include <chrono>

using namespace galay::http;
using namespace galay::kernel;

static std::atomic<bool> g_running{true};
static std::atomic<uint64_t> g_request_count{0};
static std::atomic<uint64_t> g_error_count{0};
static std::chrono::steady_clock::time_point g_start_time;

void signalHandler(int) {
    g_running = false;
}

/**
 * @brief HTTP 请求处理器 - 简单的 echo 响应
 */
Coroutine handleHttpRequest(HttpConn conn) {
    while(true) {
        auto reader = conn.getReader();
        HttpRequest request;

        while (true) {
            auto read_result = co_await reader.getRequest(request);
            if (!read_result) {
                std::cerr << "Failed to send response: " << read_result.error().message() << "\n";
                g_error_count++;
                co_return;
            }
            if (read_result.value()) break;
        }
        g_request_count++;

        // 构建响应
        auto response = Http1_1ResponseBuilder()
            .status(HttpStatusCode::OK_200)
            .header("Content-Type", "text/plain")
            .header("Connection", "keep-alive")
            .body("OK")
            .buildMove();

        auto writer = conn.getWriter();

        while (true) {
            auto result = co_await writer.sendResponse(response);
            if (!result) {
                std::cerr << "Failed to send response: " << result.error().message() << "\n";
                g_error_count++;
                break;
            }
            if (result.value()) break;
        }
    }

    co_return;
}

/**
 * @brief 打印统计信息
 */
void printStats() {
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - g_start_time).count();

    if (duration > 0) {
        uint64_t requests = g_request_count.load();
        uint64_t errors = g_error_count.load();
        double qps = static_cast<double>(requests) / duration;

        std::cout << "\r[Stats] Requests: " << requests
                  << " | Errors: " << errors
                  << " | QPS: " << static_cast<uint64_t>(qps)
                  << " | Uptime: " << duration << "s" << std::flush;
    }
}

int main(int argc, char* argv[]) {
    uint16_t port = 8080;
    if (argc > 1) {
        port = std::atoi(argv[1]);
    }

    std::cout << "========================================\n";
    std::cout << "HTTP Server Benchmark\n";
    std::cout << "========================================\n";
    std::cout << "Port: " << port << "\n";
    std::cout << "Endpoint: http://127.0.0.1:" << port << "/\n";
    std::cout << "\nBenchmark commands:\n";
    std::cout << "  wrk -t4 -c100 -d30s --latency http://127.0.0.1:" << port << "/\n";
    std::cout << "  wrk -t8 -c500 -d30s --latency http://127.0.0.1:" << port << "/\n";
    std::cout << "\nPress Ctrl+C to stop\n";
    std::cout << "========================================\n\n";

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try {
        HttpServerConfig config;
        config.host = "0.0.0.0";
        config.port = port;
        config.io_scheduler_count = 4;
        config.compute_scheduler_count = 0;

        HttpServer server(config);

        HTTP_LOG_INFO("[server] [listen] [http] [{}:{}]", config.host, config.port);

        server.start(handleHttpRequest);

        std::cout << "Server started successfully!\n";
        std::cout << "Waiting for requests...\n\n";

        g_start_time = std::chrono::steady_clock::now();

        // 定期打印统计信息
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            printStats();
        }

        std::cout << "\n\nShutting down...\n";

        auto end_time = std::chrono::steady_clock::now();
        auto total_duration = std::chrono::duration_cast<std::chrono::seconds>(end_time - g_start_time).count();

        std::cout << "\n========================================\n";
        std::cout << "Final Statistics\n";
        std::cout << "========================================\n";
        std::cout << "Total requests: " << g_request_count.load() << "\n";
        std::cout << "Total errors:   " << g_error_count.load() << "\n";
        std::cout << "Total time:     " << total_duration << " seconds\n";
        if (total_duration > 0) {
            std::cout << "Average QPS:    " << (g_request_count.load() / total_duration) << "\n";
        }
        std::cout << "========================================\n";

        server.stop();
        std::cout << "Server stopped.\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
