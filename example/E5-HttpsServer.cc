/**
 * @file E5-HttpsServer.cc
 * @brief HTTPS 服务器示例
 * @details 演示如何使用 HttpsServer 创建一个支持 Keep-Alive 的 HTTPS 服务器
 */

#include "galay-http/kernel/http/HttpServer.h"
#include "galay-http/utils/Http1_1ResponseBuilder.h"
#include <iostream>
#include <csignal>

using namespace galay::http;
using namespace galay::kernel;

#ifdef GALAY_HTTP_SSL_ENABLED

static std::atomic<bool> g_running{true};
static std::atomic<uint64_t> g_requests{0};

void signalHandler(int) {
    g_running = false;
}

// HTTPS 请求处理器 (支持 Keep-Alive)
Coroutine httpsHandler(HttpConnImpl<galay::ssl::SslSocket> conn) {
    auto reader = conn.getReader();
    auto writer = conn.getWriter();

    while (true) {
        HttpRequest request;

        // 读取请求
        while (true) {
            auto r = co_await reader.getRequest(request);
            if (!r) {
                co_await conn.close();
                co_return;
            }
            if (r.value()) break;
        }

        g_requests++;

        // 检查 keep-alive
        bool keep_alive = true;
        auto conn_header = request.header().headerPairs().getValue("Connection");
        if (conn_header == "close") {
            keep_alive = false;
        }

        // 根据 URI 返回不同响应
        std::string uri = request.header().uri();
        HttpResponse response;

        if (uri == "/echo") {
            std::string body = request.getBodyStr();
            response = Http1_1ResponseBuilder::ok()
                .header("Server", "Galay-HTTPS/1.0")
                .header("Connection", keep_alive ? "keep-alive" : "close")
                .text(body.empty() ? "Echo: (empty)" : "Echo: " + body)
                .build();
        } else {
            std::string html = R"(<!DOCTYPE html>
<html>
<head><meta charset="UTF-8"><title>HTTPS Server</title></head>
<body>
    <h1>Welcome to HTTPS Server</h1>
    <p>This is a secure connection using TLS.</p>
    <h2>Test:</h2>
    <pre>curl -k https://localhost:8443/echo -d "Hello"</pre>
</body>
</html>)";
            response = Http1_1ResponseBuilder::ok()
                .header("Server", "Galay-HTTPS/1.0")
                .header("Connection", keep_alive ? "keep-alive" : "close")
                .html(html)
                .build();
        }

        // 发送响应
        while (true) {
            auto r = co_await writer.sendResponse(response);
            if (!r || r.value()) break;
        }

        if (!keep_alive) break;
        request = HttpRequest();
    }

    co_await conn.close();
    co_return;
}

int main(int argc, char* argv[]) {
    int port = 8443;
    std::string cert_path = "test.crt";
    std::string key_path = "test.key";

    if (argc > 1) port = std::atoi(argv[1]);
    if (argc > 2) cert_path = argv[2];
    if (argc > 3) key_path = argv[3];

    std::cout << "========================================\n";
    std::cout << "HTTPS Server Example\n";
    std::cout << "========================================\n";

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try {
        HttpsServerConfig config;
        config.host = "0.0.0.0";
        config.port = port;
        config.cert_path = cert_path;
        config.key_path = key_path;
        config.io_scheduler_count = 4;

        HttpsServer server(config);

        std::cout << "Server running on https://0.0.0.0:" << port << "\n";
        std::cout << "Test: curl -k https://localhost:" << port << "/\n";
        std::cout << "Press Ctrl+C to stop\n";
        std::cout << "========================================\n";

        server.start(httpsHandler);

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

#else

int main() {
    std::cout << "SSL support is not enabled.\n";
    std::cout << "Rebuild with -DGALAY_HTTP_ENABLE_SSL=ON\n";
    return 0;
}

#endif
