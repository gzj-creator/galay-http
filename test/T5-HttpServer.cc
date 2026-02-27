/**
 * @file test_http_server.cc
 * @brief HTTP Server 测试
 */

#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <cctype>
#include "galay-http/kernel/http/HttpServer.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/utils/Http1_1ResponseBuilder.h"
#include "galay-kernel/common/Log.h"

#ifdef USE_KQUEUE
#include "galay-kernel/kernel/KqueueScheduler.h"
using IOSchedulerType = galay::kernel::KqueueScheduler;
#endif

#ifdef USE_EPOLL
#include "galay-kernel/kernel/EpollScheduler.h"
using IOSchedulerType = galay::kernel::EpollScheduler;
#endif

#ifdef USE_IOURING
#include "galay-kernel/kernel/IOUringScheduler.h"
using IOSchedulerType = galay::kernel::IOUringScheduler;
#endif

using namespace galay::http;
using namespace galay::kernel;

std::atomic<bool> g_server_running{false};
std::atomic<int> g_request_count{0};

// HTTP请求处理器协程
Coroutine handleRequest(HttpConn conn) {
    auto reader = conn.getReader();
    auto writer = conn.getWriter();

    while (true) {
        // 读取HTTP请求
        HttpRequest request;
        bool requestComplete = false;

        while (!requestComplete) {
            auto result = co_await reader.getRequest(request);

            if (!result) {
                auto& error = result.error();
                if (error.code() == kConnectionClose) {
                    LogInfo("Client disconnected");
                } else {
                    LogError("Request parse error: {}", error.message());
                }
                co_await conn.close();
                co_return;
            }

            requestComplete = result.value();
        }

        int req_no = g_request_count.fetch_add(1) + 1;

        LogInfo("Request #{} received: {} {}",
                req_no,
                static_cast<int>(request.header().method()),
                request.header().uri());

        // 根据不同的路径返回不同的内容
        HttpStatusCode code = HttpStatusCode::OK_200;
        std::string content_type = "text/html; charset=utf-8";
        std::string body;
        std::string uri = request.header().uri();

        if (uri == "/" || uri == "/index.html") {
            body = R"(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>Galay HTTP Server</title>
</head>
<body>
    <h1>Welcome to Galay HTTP Server!</h1>
    <p>This is a test page.</p>
    <ul>
        <li><a href="/hello">Hello Page</a></li>
        <li><a href="/test">Test Page</a></li>
        <li><a href="/api/info">API Info</a></li>
    </ul>
</body>
</html>)";
        } else if (uri == "/hello") {
            body = R"(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>Hello</title>
</head>
<body>
    <h1>Hello from Galay HTTP!</h1>
    <p><a href="/">Back to Home</a></p>
</body>
</html>)";
        } else if (uri == "/test") {
            body = R"(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>Test</title>
</head>
<body>
    <h1>Test Page</h1>
    <p>This is a test page for Galay HTTP Server.</p>
    <p><a href="/">Back to Home</a></p>
</body>
</html>)";
        } else if (uri == "/api/info") {
            content_type = "application/json";
            body = R"({
    "server": "galay-http",
    "version": "1.0.0",
    "status": "running",
    "timestamp": ")" + std::to_string(std::time(nullptr)) + R"("
})";
        } else {
            code = HttpStatusCode::NotFound_404;
            body = R"(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>404 Not Found</title>
</head>
<body>
    <h1>404 Not Found</h1>
    <p>The requested URL was not found on this server.</p>
    <p><a href="/">Back to Home</a></p>
</body>
</html>)";
        }

        bool keep_alive = true;
        std::string conn_hdr = request.header().headerPairs().getValue("Connection");
        if (!conn_hdr.empty()) {
            for (auto& c : conn_hdr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (conn_hdr == "close") {
                keep_alive = false;
            }
        }

        // 发送响应
        auto response = Http1_1ResponseBuilder()
            .status(code)
            .header("Content-Type", content_type)
            .header("Server", GALAY_SERVER)
            .header("Connection", keep_alive ? "keep-alive" : "close")
            .body(std::move(body))
            .buildMove();

        auto sendResult = co_await writer.sendResponse(response);
        if (!sendResult) {
            LogError("Failed to send response: {}", sendResult.error().message());
            keep_alive = false;
        } else {
            LogInfo("Response sent: complete");
        }

        if (!keep_alive) {
            break;
        }
    }

    co_await conn.close();
    co_return;
}

int main() {
    LogInfo("========================================");
    LogInfo("HTTP Server Test");
    LogInfo("========================================\n");

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
    // 配置并启动服务器
    HttpServer server(HttpServerBuilder()
        .host("127.0.0.1")
        .port(8080)
        .backlog(128)
        .build());

    g_server_running = true;
    LogInfo("========================================");
    LogInfo("HTTP Server is running on http://127.0.0.1:8080");
    LogInfo("========================================");
    LogInfo("Available endpoints:");
    LogInfo("  - http://127.0.0.1:8080/");
    LogInfo("  - http://127.0.0.1:8080/hello");
    LogInfo("  - http://127.0.0.1:8080/test");
    LogInfo("  - http://127.0.0.1:8080/api/info");
    LogInfo("========================================");
    LogInfo("Press Ctrl+C to stop the server");
    LogInfo("========================================\n");

    // 运行服务器（阻塞）
    server.start(handleRequest);
    while (g_server_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    server.stop();
    LogInfo("Server stopped");
#else
    LogWarn("This test requires kqueue (macOS), epoll or io_uring (Linux)");
    return 1;
#endif

    return 0;
}
