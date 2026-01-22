/**
 * @file test_static_file_server.cc
 * @brief 静态文件服务测试 - 测试 mount() 和 mountHardly() 功能
 */

#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <algorithm>
#include "galay-http/kernel/http/HttpServer.h"
#include "galay-http/kernel/http/HttpRouter.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/utils/HttpUtils.h"
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

// 全局路由器
HttpRouter g_router;

// API 处理器
Coroutine apiHandler(HttpConn& conn, HttpRequest req) {
    g_request_count++;

    LogInfo("API Request #{}: {} {}",
            g_request_count.load(),
            static_cast<int>(req.header().method()),
            req.header().uri());

    // 创建响应
    HttpResponse response;
    response.header().version() = HttpVersion::HttpVersion_1_1;
    response.header().code() = HttpStatusCode::OK_200;
    response.header().headerPairs().addHeaderPair("Content-Type", "application/json");
    response.header().headerPairs().addHeaderPair("Server", GALAY_SERVER);

    std::string body = R"({
    "status": "ok",
    "message": "Static file server is running",
    "request_count": )" + std::to_string(g_request_count.load()) + R"(,
    "endpoints": {
        "dynamic": "/static/**",
        "static": "/files/**",
        "api": "/api/status"
    }
})";

    response.setBodyStr(std::move(body));

    // 发送响应
    auto writer = conn.getWriter();
    auto result = co_await writer.sendResponse(response);

    if (!result) {
        LogError("Failed to send response: {}", result.error().message());
    }

    co_await conn.close();
    co_return;
}

// 主页处理器
Coroutine indexHandler(HttpConn& conn, HttpRequest req) {
    g_request_count++;

    LogInfo("Index Request #{}: {} {}",
            g_request_count.load(),
            static_cast<int>(req.header().method()),
            req.header().uri());

    HttpResponse response;
    response.header().version() = HttpVersion::HttpVersion_1_1;
    response.header().code() = HttpStatusCode::OK_200;
    response.header().headerPairs().addHeaderPair("Content-Type", "text/html; charset=utf-8");
    response.header().headerPairs().addHeaderPair("Server", GALAY_SERVER);

    std::string body = R"(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>Static File Server Test</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 40px; }
        h1 { color: #333; }
        .section { margin: 20px 0; padding: 15px; background: #f5f5f5; border-radius: 5px; }
        a { color: #0066cc; text-decoration: none; }
        a:hover { text-decoration: underline; }
    </style>
</head>
<body>
    <h1>Static File Server Test</h1>

    <div class="section">
        <h2>Dynamic Mount (mount)</h2>
        <p>Files served dynamically from disk:</p>
        <ul>
            <li><a href="/static/index.html">HTML File</a></li>
            <li><a href="/static/css/style.css">CSS File</a></li>
            <li><a href="/static/js/app.js">JavaScript File</a></li>
            <li><a href="/static/docs/test.txt">Text File</a></li>
            <li><a href="/static/docs/data.json">JSON File</a></li>
        </ul>
    </div>

    <div class="section">
        <h2>Static Mount (mountHardly)</h2>
        <p>Files pre-loaded into memory:</p>
        <ul>
            <li><a href="/files/index.html">HTML File</a></li>
            <li><a href="/files/css/style.css">CSS File</a></li>
            <li><a href="/files/js/app.js">JavaScript File</a></li>
            <li><a href="/files/docs/test.txt">Text File</a></li>
            <li><a href="/files/docs/data.json">JSON File</a></li>
        </ul>
    </div>

    <div class="section">
        <h2>Performance Test Files</h2>
        <ul>
            <li><a href="/static/small.bin">Small File (10KB)</a></li>
            <li><a href="/static/medium.bin">Medium File (1MB)</a></li>
            <li><a href="/static/large.bin">Large File (10MB)</a></li>
        </ul>
    </div>

    <div class="section">
        <h2>API</h2>
        <ul>
            <li><a href="/api/status">Server Status</a></li>
        </ul>
    </div>
</body>
</html>)";

    response.setBodyStr(std::move(body));

    auto writer = conn.getWriter();
    auto result = co_await writer.sendResponse(response);

    if (!result) {
        LogError("Failed to send response: {}", result.error().message());
    }

    co_await conn.close();
    co_return;
}

// 使用路由器的请求处理器 - 测试 mount 和 mountHardly
int main(int argc, char* argv[]) {
    std::string static_dir = "./test/static_files";
    int port = 8080;

    // 解析命令行参数
    if (argc > 1) {
        static_dir = argv[1];
    }
    if (argc > 2) {
        port = std::atoi(argv[2]);
    }

    LogInfo("========================================");
    LogInfo("Static File Server Test");
    LogInfo("========================================");
    LogInfo("Static directory: {}", static_dir);
    LogInfo("Server port: {}", port);
    LogInfo("========================================\n");

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
    try {
        // 注册主页路由
        g_router.addHandler<HttpMethod::GET>("/", indexHandler);

        // 注册 API 路由
        g_router.addHandler<HttpMethod::GET>("/api/status", apiHandler);

        // 挂载静态文件目录 - 动态模式（从磁盘读取）
        LogInfo("Mounting static files (dynamic mode): /static -> {}", static_dir);
        g_router.mount("/static", static_dir);

        // 挂载静态文件目录 - 静态模式（预加载到内存）
        LogInfo("Mounting static files (static mode): /files -> {}", static_dir);
        g_router.mountHardly("/files", static_dir);

        LogInfo("Router has {} routes registered\n", g_router.size());

        // 配置并启动服务器
        HttpServerConfig server_config;
        server_config.host = "0.0.0.0";
        server_config.port = port;
        server_config.backlog = 128;

        HttpServer server(server_config);

        g_server_running = true;

        LogInfo("========================================");
        LogInfo("HTTP Server is running on http://0.0.0.0:{}", port);
        LogInfo("========================================");
        LogInfo("Test URLs:");
        LogInfo("  - http://localhost:{}/", port);
        LogInfo("  - http://localhost:{}/api/status", port);
        LogInfo("  - http://localhost:{}/static/index.html", port);
        LogInfo("  - http://localhost:{}/files/index.html", port);
        LogInfo("  - http://localhost:{}/static/small.bin", port);
        LogInfo("  - http://localhost:{}/static/medium.bin", port);
        LogInfo("  - http://localhost:{}/static/large.bin", port);
        LogInfo("========================================");
        LogInfo("Press Ctrl+C to stop the server");
        LogInfo("========================================\n");

        // 使用新的 API：将 Router 移动到 Server 内部，Server 自动处理路由
        server.start(std::move(g_router));

        while (g_server_running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        server.stop();
        LogInfo("Server stopped");

    } catch (const std::exception& e) {
        LogError("Server error: {}", e.what());
        return 1;
    }
#else
    LogWarn("This test requires kqueue (macOS), epoll or io_uring (Linux)");
    return 1;
#endif

    return 0;
}
