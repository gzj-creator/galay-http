/**
 * @file T21-HttpsServer.cc
 * @brief HTTPS 服务器测试 - 支持 keep-alive
 */

#include "galay-http/kernel/http/HttpServer.h"
#include "galay-http/utils/Http1_1ResponseBuilder.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>

using namespace galay::http;
using namespace galay::kernel;

#ifdef GALAY_HTTP_SSL_ENABLED

static std::atomic<bool> g_running{true};
static std::atomic<uint64_t> g_request_count{0};

void signalHandler(int) {
    g_running = false;
}

// HTTPS 请求处理器 - 支持 keep-alive
Coroutine httpsHandler(HttpConnImpl<galay::ssl::SslSocket> conn) {
    auto reader = conn.getReader();
    auto writer = conn.getWriter();

    while (true) {
        HttpRequest request;

        // 读取请求
        while (true) {
            auto read_result = co_await reader.getRequest(request);
            if (!read_result) {
                // 连接关闭或错误，退出循环
                co_await conn.close();
                co_return;
            }
            if (read_result.value()) break;
        }

        g_request_count++;

        // 检查是否是 keep-alive
        bool keep_alive = true;
        auto connection_header = request.header().headerPairs().getValue("Connection");
        if (connection_header == "close") {
            keep_alive = false;
        }

        // 构建响应
        auto response = Http1_1ResponseBuilder::ok()
            .header("Server", "Galay-HTTPS/1.0")
            .header("Connection", keep_alive ? "keep-alive" : "close")
            .header("Keep-Alive", "timeout=30, max=1000")
            .text("Hello from HTTPS server!\n")
            .build();

        // 发送响应
        while (true) {
            auto send_result = co_await writer.sendResponse(response);
            if (!send_result) {
                co_await conn.close();
                co_return;
            }
            if (send_result.value()) break;
        }

        // 如果不是 keep-alive，关闭连接
        if (!keep_alive) {
            break;
        }

        // 重置 request 以便下次使用
        request = HttpRequest();
    }

    // 执行 SSL shutdown
    for (int i = 0; i < 10; i++) {
        auto shutdown_result = co_await conn.shutdown();
        if (shutdown_result) break;
    }

    co_await conn.close();
    co_return;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "HTTPS Server Test (Keep-Alive)" << std::endl;
    std::cout << "========================================" << std::endl;

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    HttpsServerConfig config;
    config.host = "0.0.0.0";
    config.port = 8443;
    config.cert_path = "test.crt";
    config.key_path = "test.key";
    config.io_scheduler_count = 8;
    config.compute_scheduler_count = 0;

    try {
        HttpsServer server(config);

        std::cout << "Starting HTTPS server on port " << config.port << "..." << std::endl;
        server.start(httpsHandler);

        std::cout << "HTTPS server started successfully!" << std::endl;
        std::cout << "Test with: curl -k https://localhost:8443/" << std::endl;
        std::cout << "Press Ctrl+C to stop..." << std::endl;

        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "\nTotal requests handled: " << g_request_count << std::endl;
        server.stop();
        std::cout << "HTTPS server stopped." << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}

#else

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "HTTPS Server Test" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "SSL support is not enabled." << std::endl;
    std::cout << "Rebuild with -DGALAY_HTTP_ENABLE_SSL=ON" << std::endl;
    return 0;
}

#endif
