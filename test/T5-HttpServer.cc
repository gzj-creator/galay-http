/**
 * @file test_http_server.cc
 * @brief HTTP Server 测试
 */

#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include "galay-http/kernel/http/HttpServer.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
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
    g_request_count++;

    // 获取 Reader 和 Writer
    auto reader = conn.getReader();
    auto writer = conn.getWriter();

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

    LogInfo("Request #{} received: {} {}",
            g_request_count.load(),
            static_cast<int>(request.header().method()),
            request.header().uri());

    // 创建响应
    HttpResponseHeader resp_header;
    resp_header.version() = HttpVersion::HttpVersion_1_1;
    resp_header.code() = HttpStatusCode::OK_200;
    resp_header.headerPairs().addHeaderPair("Content-Type", "text/html; charset=utf-8");
    resp_header.headerPairs().addHeaderPair("Server", GALAY_SERVER);

    // 根据不同的路径返回不同的内容
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
        resp_header.headerPairs().removeHeaderPair("Content-Type");
        resp_header.headerPairs().addHeaderPair("Content-Type", "application/json");
        body = R"({
    "server": "galay-http",
    "version": "1.0.0",
    "status": "running",
    "timestamp": ")" + std::to_string(std::time(nullptr)) + R"("
})";
    } else {
        resp_header.code() = HttpStatusCode::NotFound_404;
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

    resp_header.headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));

    // 发送响应
    HttpResponse response;
    response.setHeader(std::move(resp_header));
    response.setBodyStr(std::move(body));

    while (true) {
        auto sendResult = co_await writer.sendResponse(response);
        if (!sendResult) {
            LogError("Failed to send response: {}", sendResult.error().message());
            break;
        }
        if (sendResult.value()) {
            LogInfo("Response sent: complete");
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
    HttpServerConfig server_config;
    server_config.host = "127.0.0.1";
    server_config.port = 8080;
    server_config.backlog = 128;

    HttpServer server(server_config);

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
