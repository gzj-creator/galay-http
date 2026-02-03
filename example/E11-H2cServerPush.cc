/**
 * @file E11-H2cServerPush.cc
 * @brief h2c Server Push 示例
 * @details 演示如何使用 HTTP/2 Server Push 功能
 * 
 * 测试方法:
 *   # 使用 nghttp (会显示推送的资源)
 *   nghttp -v http://localhost:8080/
 *   
 *   # 使用 curl (curl 不支持显示推送)
 *   curl --http2-prior-knowledge -v http://localhost:8080/
 */

#include "galay-http/kernel/http2/Http2Server.h"
#include <iostream>
#include <csignal>

using namespace galay::http2;
using namespace galay::kernel;

static std::atomic<bool> g_running{true};

void signalHandler(int) {
    g_running = false;
}

// 静态资源
static const char* CSS_CONTENT = R"(
body {
    font-family: 'Segoe UI', Arial, sans-serif;
    max-width: 800px;
    margin: 50px auto;
    padding: 20px;
    background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
    min-height: 100vh;
}
.container {
    background: white;
    padding: 30px;
    border-radius: 10px;
    box-shadow: 0 10px 40px rgba(0,0,0,0.2);
}
h1 { color: #333; margin-bottom: 20px; }
.pushed { color: #28a745; font-weight: bold; }
pre { background: #f8f9fa; padding: 15px; border-radius: 5px; }
)";

static const char* JS_CONTENT = R"(
console.log('JavaScript loaded via Server Push!');
document.addEventListener('DOMContentLoaded', function() {
    var el = document.getElementById('js-status');
    if (el) {
        el.textContent = 'JavaScript executed successfully!';
        el.className = 'pushed';
    }
});
)";

// 发送响应的辅助函数
Coroutine sendResponse(Http2ConnImpl<galay::async::TcpSocket>& conn, uint32_t stream_id,
                       int status, const std::string& content_type, const std::string& body) {
    std::vector<Http2HeaderField> headers;
    headers.push_back({":status", std::to_string(status)});
    headers.push_back({"content-type", content_type});
    headers.push_back({"server", "Galay-H2c-Push/1.0"});

    bool has_body = !body.empty();

    // 发送 HEADERS（循环直到完成）
    while (true) {
        auto result = co_await conn.sendHeaders(stream_id, headers, !has_body, true);
        if (!result) co_return;
        if (result.value()) break;
    }

    // 发送 DATA（如果有，循环直到完成）
    if (has_body) {
        while (true) {
            auto result = co_await conn.sendDataFrame(stream_id, body, true);
            if (!result) co_return;
            if (result.value()) break;
        }
    }

    co_return;
}

// HTTP/2 请求处理器（带 Server Push）
Coroutine h2cPushHandler(Http2ConnImpl<galay::async::TcpSocket>& conn, Http2Stream::ptr stream, Http2Request request) {
    HTTP_LOG_INFO("Request: {} {} (stream {})", request.method, request.path, stream->streamId());

    if (request.path == "/style.css") {
        // CSS 资源
        co_await sendResponse(conn, stream->streamId(), 200, "text/css", CSS_CONTENT).wait();
    }
    else if (request.path == "/script.js") {
        // JavaScript 资源
        co_await sendResponse(conn, stream->streamId(), 200, "application/javascript", JS_CONTENT).wait();
    }
    else if (request.path == "/" || request.path == "/index.html") {
        // 主页面 - 推送 CSS 和 JS

        // 发送 PUSH_PROMISE for CSS
        auto [css_stream_id, css_push_awaitable] = conn.preparePushPromise(
            stream->streamId(), "GET", "/style.css", request.authority, "http");

        if (css_push_awaitable) {
            HTTP_LOG_INFO("Pushing /style.css on stream {}", css_stream_id);
            while (true) {
                auto result = co_await *css_push_awaitable;
                if (!result || result.value()) break;
            }

            // 发送推送的 CSS 响应
            co_await sendResponse(conn, css_stream_id, 200, "text/css", CSS_CONTENT).wait();
        }

        // 发送 PUSH_PROMISE for JS
        auto [js_stream_id, js_push_awaitable] = conn.preparePushPromise(
            stream->streamId(), "GET", "/script.js", request.authority, "http");

        if (js_push_awaitable) {
            HTTP_LOG_INFO("Pushing /script.js on stream {}", js_stream_id);
            while (true) {
                auto result = co_await *js_push_awaitable;
                if (!result || result.value()) break;
            }

            // 发送推送的 JS 响应
            co_await sendResponse(conn, js_stream_id, 200, "application/javascript", JS_CONTENT).wait();
        }

        // 发送主页面响应
        std::string html = R"(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>HTTP/2 Server Push Demo</title>
    <link rel="stylesheet" href="/style.css">
    <script src="/script.js"></script>
</head>
<body>
    <div class="container">
        <h1>HTTP/2 Server Push Demo</h1>

        <p>This page demonstrates HTTP/2 Server Push. The following resources
        were pushed along with this HTML:</p>

        <ul>
            <li><span class="pushed">/style.css</span> - Stylesheet (pushed)</li>
            <li><span class="pushed">/script.js</span> - JavaScript (pushed)</li>
        </ul>

        <h2>JavaScript Status</h2>
        <p id="js-status">Waiting for JavaScript...</p>

        <h2>How to Test</h2>
        <pre>
# Use nghttp to see pushed resources:
nghttp -v http://localhost:8080/

# Output will show PUSH_PROMISE frames
        </pre>

        <h2>Benefits of Server Push</h2>
        <ul>
            <li>Resources arrive before browser parses HTML</li>
            <li>Eliminates round-trip for critical resources</li>
            <li>Improves page load time</li>
        </ul>
    </div>
</body>
</html>)";

        co_await sendResponse(conn, stream->streamId(), 200, "text/html; charset=utf-8", html).wait();
    }
    else {
        // 404
        co_await sendResponse(conn, stream->streamId(), 404, "text/plain", "404 Not Found").wait();
    }

    co_return;
}

int main(int argc, char* argv[]) {
    int port = 8080;
    if (argc > 1) {
        port = std::atoi(argv[1]);
    }
    
    std::cout << "========================================\n";
    std::cout << "HTTP/2 Server Push Example\n";
    std::cout << "========================================\n";
    
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    try {
        H2cServerConfig config;
        config.host = "0.0.0.0";
        config.port = port;
        config.io_scheduler_count = 4;
        config.enable_push = true;  // 启用 Server Push
        
        H2cServer server(config);
        
        std::cout << "Server running on http://0.0.0.0:" << port << "\n";
        std::cout << "Test with: nghttp -v http://localhost:" << port << "/\n";
        std::cout << "Press Ctrl+C to stop\n";
        std::cout << "========================================\n";
        
        server.start(h2cPushHandler);
        
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        server.stop();
        std::cout << "Server stopped.\n";
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
