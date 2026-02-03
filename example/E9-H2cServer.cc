/**
 * @file E9-H2cServer.cc
 * @brief h2c (HTTP/2 over cleartext) 服务器示例
 * @details 演示如何使用 H2cServer 创建一个 HTTP/2 明文服务器
 * 
 * 测试方法:
 *   # 使用 curl (Prior Knowledge 模式)
 *   curl --http2-prior-knowledge -v http://localhost:8080/
 *   curl --http2-prior-knowledge -v http://localhost:8080/echo -d "Hello HTTP/2"
 *   
 *   # 使用 nghttp
 *   nghttp -v http://localhost:8080/
 */

#include "galay-http/kernel/http2/Http2Server.h"
#include <iostream>
#include <csignal>

using namespace galay::http2;
using namespace galay::kernel;

static std::atomic<bool> g_running{true};
static std::atomic<uint64_t> g_requests{0};

void signalHandler(int) {
    g_running = false;
}

// HTTP/2 请求处理器
Coroutine h2cHandler(Http2ConnImpl<galay::async::TcpSocket>& conn, Http2Stream::ptr stream, Http2Request request) {
    g_requests++;
    
    HTTP_LOG_INFO("H2c request: {} {} (stream {})", 
                  request.method, request.path, stream->streamId());
    
    Http2Response response;
    
    if (request.path == "/echo") {
        // Echo 端点
        response.setStatus(200);
        response.setHeader("content-type", "text/plain");
        response.setHeader("server", "Galay-H2c/1.0");
        
        std::string body = request.body.empty() 
            ? "Echo: (empty body)" 
            : "Echo: " + request.body;
        response.setBody(body);
    } 
    else if (request.path == "/json") {
        // JSON 端点
        response.setStatus(200);
        response.setHeader("content-type", "application/json");
        response.setHeader("server", "Galay-H2c/1.0");
        response.setBody(R"({"message":"Hello from HTTP/2","protocol":"h2c"})");
    }
    else {
        // 默认页面
        response.setStatus(200);
        response.setHeader("content-type", "text/html; charset=utf-8");
        response.setHeader("server", "Galay-H2c/1.0");
        
        std::string html = R"(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>H2c Server</title>
    <style>
        body { font-family: Arial, sans-serif; max-width: 800px; margin: 50px auto; padding: 20px; }
        h1 { color: #333; }
        pre { background: #f4f4f4; padding: 15px; border-radius: 5px; overflow-x: auto; }
        .info { color: #666; }
    </style>
</head>
<body>
    <h1>Welcome to H2c Server</h1>
    <p class="info">This is an HTTP/2 cleartext (h2c) server.</p>
    
    <h2>Request Info</h2>
    <pre>
Method: )" + request.method + R"(
Path: )" + request.path + R"(
Authority: )" + request.authority + R"(
Scheme: )" + request.scheme + R"(
    </pre>
    
    <h2>Test Endpoints</h2>
    <ul>
        <li><code>/</code> - This page</li>
        <li><code>/echo</code> - Echo POST body</li>
        <li><code>/json</code> - JSON response</li>
    </ul>
    
    <h2>Test Commands</h2>
    <pre>
# GET request
curl --http2-prior-knowledge http://localhost:8080/

# POST with body
curl --http2-prior-knowledge http://localhost:8080/echo -d "Hello"

# JSON endpoint
curl --http2-prior-knowledge http://localhost:8080/json

# Using nghttp
nghttp -v http://localhost:8080/
    </pre>
</body>
</html>)";
        response.setBody(html);
    }
    
    // 构建响应头部
    std::vector<Http2HeaderField> headers;
    headers.push_back({":status", std::to_string(response.status)});
    for (const auto& h : response.headers) {
        headers.push_back(h);
    }
    
    bool has_body = !response.body.empty();

    // 发送 HEADERS（循环直到完成）
    HTTP_LOG_INFO("Sending HEADERS for stream {}, headers count: {}, end_stream: {}",
                  stream->streamId(), headers.size(), !has_body);
    while (true) {
        auto result = co_await conn.sendHeaders(stream->streamId(), headers, !has_body, true);
        if (!result) {
            HTTP_LOG_ERROR("failed to send headers: {}", http2ErrorCodeToString(result.error()));
            co_return;
        }
        HTTP_LOG_DEBUG("sendHeaders returned: {}", result.value());
        if (result.value()) break;
    }
    HTTP_LOG_INFO("HEADERS sent successfully for stream {}", stream->streamId());

    // 发送 DATA（如果有，循环直到完成）
    if (has_body) {
        HTTP_LOG_INFO("Sending DATA for stream {}, body size: {}", stream->streamId(), response.body.size());
        while (true) {
            auto result = co_await conn.sendDataFrame(stream->streamId(), response.body, true);
            if (!result) {
                HTTP_LOG_ERROR("failed to send data: {}", http2ErrorCodeToString(result.error()));
                co_return;
            }
            HTTP_LOG_DEBUG("sendDataFrame returned: {}", result.value());
            if (result.value()) break;
        }
        HTTP_LOG_INFO("DATA sent successfully for stream {}", stream->streamId());
    }

    co_return;
}

int main(int argc, char* argv[]) {
    int port = 8080;
    if (argc > 1) {
        port = std::atoi(argv[1]);
    }
    
    std::cout << "========================================\n";
    std::cout << "H2c (HTTP/2 Cleartext) Server Example\n";
    std::cout << "========================================\n";
    
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    try {
        H2cServerConfig config;
        config.host = "0.0.0.0";
        config.port = port;
        config.io_scheduler_count = 4;
        config.max_concurrent_streams = 100;
        config.enable_push = false;  // curl 不支持 Server Push
        
        H2cServer server(config);
        
        std::cout << "Server running on http://0.0.0.0:" << port << "\n";
        std::cout << "Test: curl --http2-prior-knowledge http://localhost:" << port << "/\n";
        std::cout << "Press Ctrl+C to stop\n";
        std::cout << "========================================\n";
        
        server.start(h2cHandler);
        
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
