/**
 * @file example_http_router_integration.cc
 * @brief HttpRouter 完整集成示例
 * @details 展示如何将 HttpRouter 与 HttpServer 集成，并使用路由参数
 */

#include <memory>
#include "galay-http/kernel/http/HttpServer.h"
#include "galay-http/kernel/http/HttpRouter.h"
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

// 全局路由器
std::unique_ptr<HttpRouter> g_router;

// ==================== 路由处理器 ====================

// 首页处理器
Coroutine indexHandler(HttpConn& conn, HttpRequest request) {
    auto writer = conn.getWriter();

    HttpResponseHeader resp_header;
    resp_header.version() = HttpVersion::HttpVersion_1_1;
    resp_header.code() = HttpStatusCode::OK_200;
    resp_header.headerPairs().addHeaderPair("Content-Type", "text/html; charset=utf-8");

    std::string body = R"(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>HttpRouter Example</title>
</head>
<body>
    <h1>Welcome to HttpRouter Example!</h1>
    <h2>Available Routes:</h2>
    <ul>
        <li><a href="/api/users">GET /api/users</a> - List all users</li>
        <li><a href="/api/users/123">GET /api/users/:id</a> - Get user by ID</li>
        <li><a href="/api/users/123/posts">GET /api/users/:userId/posts</a> - Get user's posts</li>
        <li><a href="/api/users/123/posts/456">GET /api/users/:userId/posts/:postId</a> - Get specific post</li>
        <li><a href="/static/css/style.css">GET /static/**</a> - Static files</li>
    </ul>
</body>
</html>)";

    resp_header.headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));

    HttpResponse response;
    response.setHeader(std::move(resp_header));
    response.setBodyStr(std::move(body));

    auto result = co_await writer.sendResponse(response);
    if (!result) {
        LogError("Failed to send response: {}", result.error().message());
    }

    co_return;
}

// 获取所有用户
Coroutine getUsersHandler(HttpConn& conn, HttpRequest request) {
    auto writer = conn.getWriter();

    HttpResponseHeader resp_header;
    resp_header.version() = HttpVersion::HttpVersion_1_1;
    resp_header.code() = HttpStatusCode::OK_200;
    resp_header.headerPairs().addHeaderPair("Content-Type", "application/json");

    std::string body = R"({
    "users": [
        {"id": 1, "name": "Alice"},
        {"id": 2, "name": "Bob"},
        {"id": 3, "name": "Charlie"}
    ]
})";

    resp_header.headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));

    HttpResponse response;
    response.setHeader(std::move(resp_header));
    response.setBodyStr(std::move(body));

    co_await writer.sendResponse(response);
    co_return;
}

// 获取单个用户（使用路由参数）
Coroutine getUserByIdHandler(HttpConn& conn, HttpRequest request) {
    auto writer = conn.getWriter();

    // 从请求中获取路由参数
    std::string userId = request.getRouteParam("id", "unknown");

    LogInfo("Getting user with ID: {}", userId);

    HttpResponseHeader resp_header;
    resp_header.version() = HttpVersion::HttpVersion_1_1;
    resp_header.code() = HttpStatusCode::OK_200;
    resp_header.headerPairs().addHeaderPair("Content-Type", "application/json");

    std::string body = R"({
    "id": )" + userId + R"(,
    "name": "User )" + userId + R"(",
    "email": "user)" + userId + R"(@example.com"
})";

    resp_header.headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));

    HttpResponse response;
    response.setHeader(std::move(resp_header));
    response.setBodyStr(std::move(body));

    co_await writer.sendResponse(response);
    co_return;
}

// 获取用户的文章列表（多个路由参数）
Coroutine getUserPostsHandler(HttpConn& conn, HttpRequest request) {
    auto writer = conn.getWriter();

    // 获取多个路由参数
    std::string userId = request.getRouteParam("userId", "unknown");

    LogInfo("Getting posts for user: {}", userId);

    HttpResponseHeader resp_header;
    resp_header.version() = HttpVersion::HttpVersion_1_1;
    resp_header.code() = HttpStatusCode::OK_200;
    resp_header.headerPairs().addHeaderPair("Content-Type", "application/json");

    std::string body = R"({
    "userId": )" + userId + R"(,
    "posts": [
        {"id": 1, "title": "Post 1"},
        {"id": 2, "title": "Post 2"}
    ]
})";

    resp_header.headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));

    HttpResponse response;
    response.setHeader(std::move(resp_header));
    response.setBodyStr(std::move(body));

    co_await writer.sendResponse(response);
    co_return;
}

// 获取用户的特定文章（多个路由参数）
Coroutine getUserPostByIdHandler(HttpConn& conn, HttpRequest request) {
    auto writer = conn.getWriter();

    // 获取多个路由参数
    std::string userId = request.getRouteParam("userId", "unknown");
    std::string postId = request.getRouteParam("postId", "unknown");

    LogInfo("Getting post {} for user {}", postId, userId);

    HttpResponseHeader resp_header;
    resp_header.version() = HttpVersion::HttpVersion_1_1;
    resp_header.code() = HttpStatusCode::OK_200;
    resp_header.headerPairs().addHeaderPair("Content-Type", "application/json");

    std::string body = R"({
    "userId": )" + userId + R"(,
    "postId": )" + postId + R"(,
    "title": "Post )" + postId + R"( by User )" + userId + R"(",
    "content": "This is the content of post )" + postId + R"("
})";

    resp_header.headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));

    HttpResponse response;
    response.setHeader(std::move(resp_header));
    response.setBodyStr(std::move(body));

    co_await writer.sendResponse(response);
    co_return;
}

// 静态文件处理器（通配符匹配）
Coroutine staticFilesHandler(HttpConn& conn, HttpRequest request) {
    auto writer = conn.getWriter();

    std::string path = request.header().uri();
    LogInfo("Serving static file: {}", path);

    HttpResponseHeader resp_header;
    resp_header.version() = HttpVersion::HttpVersion_1_1;
    resp_header.code() = HttpStatusCode::OK_200;
    resp_header.headerPairs().addHeaderPair("Content-Type", "text/plain");

    std::string body = "Static file: " + path + "\n(This is a mock response)";

    resp_header.headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));

    HttpResponse response;
    response.setHeader(std::move(resp_header));
    response.setBodyStr(std::move(body));

    co_await writer.sendResponse(response);
    co_return;
}

// 404 处理器
Coroutine notFoundHandler(HttpConn& conn, HttpRequest request) {
    auto writer = conn.getWriter();

    HttpResponseHeader resp_header;
    resp_header.version() = HttpVersion::HttpVersion_1_1;
    resp_header.code() = HttpStatusCode::NotFound_404;
    resp_header.headerPairs().addHeaderPair("Content-Type", "text/html; charset=utf-8");

    std::string body = R"(<!DOCTYPE html>
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

    resp_header.headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));

    HttpResponse response;
    response.setHeader(std::move(resp_header));
    response.setBodyStr(std::move(body));

    co_await writer.sendResponse(response);
    co_return;
}

// ==================== 路由配置 ====================

void setupRoutes() {
    g_router = std::make_unique<HttpRouter>();

    // 首页
    g_router->addHandler<HttpMethod::GET>("/", indexHandler);

    // 用户 API
    g_router->addHandler<HttpMethod::GET>("/api/users", getUsersHandler);
    g_router->addHandler<HttpMethod::GET>("/api/users/:id", getUserByIdHandler);

    // 用户文章 API（多个参数）
    g_router->addHandler<HttpMethod::GET>("/api/users/:userId/posts", getUserPostsHandler);
    g_router->addHandler<HttpMethod::GET>("/api/users/:userId/posts/:postId", getUserPostByIdHandler);

    // 静态文件（通配符）
    g_router->addHandler<HttpMethod::GET>("/static/**", staticFilesHandler);

    LogInfo("Routes configured:");
    LogInfo("  GET /");
    LogInfo("  GET /api/users");
    LogInfo("  GET /api/users/:id");
    LogInfo("  GET /api/users/:userId/posts");
    LogInfo("  GET /api/users/:userId/posts/:postId");
    LogInfo("  GET /static/**");
    LogInfo("Total routes: {}", g_router->size());
}

// ==================== 请求处理 ====================

Coroutine handleRequest(HttpConn conn) {
    auto reader = conn.getReader();
    auto writer = conn.getWriter();

    // 读取 HTTP 请求
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

    LogInfo("Request received: {} {}",
            httpMethodToString(request.header().method()),
            request.header().uri());

    // 使用路由器查找处理器
    auto match = g_router->findHandler(
        request.header().method(),
        request.header().uri()
    );

    if (match.handler) {
        // 找到匹配的路由
        // 将路由参数设置到请求对象中
        request.setRouteParams(std::move(match.params));

        // 打印路由参数（调试用）
        if (!request.routeParams().empty()) {
            LogInfo("Route params:");
            for (const auto& [key, value] : request.routeParams()) {
                LogInfo("  {} = {}", key, value);
            }
        }

        // 调用处理器（使用 co_await Coroutine.wait()）
        co_await (*match.handler)(conn, std::move(request)).wait();
    } else {
        // 未找到匹配的路由，返回 404
        LogWarn("No route found for: {} {}",
                httpMethodToString(request.header().method()),
                request.header().uri());
        co_await notFoundHandler(conn, std::move(request)).wait();
    }

    co_await conn.close();
    co_return;
}

// ==================== 主函数 ====================

int main() {
    LogInfo("========================================");
    LogInfo("HttpRouter Integration Example");
    LogInfo("========================================\n");

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
    // 配置路由
    setupRoutes();

    // 配置服务器
    HttpServerConfig server_config;
    server_config.host = "127.0.0.1";
    server_config.port = 8080;
    server_config.backlog = 128;

    HttpServer server(server_config);

    LogInfo("\n========================================");
    LogInfo("HTTP Server is running on http://127.0.0.1:8080");
    LogInfo("========================================");
    LogInfo("Try these URLs:");
    LogInfo("  http://127.0.0.1:8080/");
    LogInfo("  http://127.0.0.1:8080/api/users");
    LogInfo("  http://127.0.0.1:8080/api/users/123");
    LogInfo("  http://127.0.0.1:8080/api/users/123/posts");
    LogInfo("  http://127.0.0.1:8080/api/users/123/posts/456");
    LogInfo("  http://127.0.0.1:8080/static/css/style.css");
    LogInfo("========================================");
    LogInfo("Press Ctrl+C to stop the server");
    LogInfo("========================================\n");

    // 启动服务器
    server.start(handleRequest);

    // 服务器运行中...
    // (start 方法会阻塞直到服务器停止)

    LogInfo("Server stopped");
#else
    LogWarn("This example requires kqueue (macOS), epoll or io_uring (Linux)");
    return 1;
#endif

    return 0;
}
