// HTTP/2 over TLS (h2) 测试服务器
// 
// 这个示例展示了如何使用 HttpsServer 通过 ALPN 协商提供 HTTP/2 服务
// 
// 编译:
//   cd build && make test_http2_server_h2
// 
// 运行:
//   cd build/test && ./test_http2_server_h2
// 
// 测试:
//   curl -v --http2 https://localhost:8443/ --insecure
//   curl -v --http2 https://localhost:8443/api/hello --insecure

// ========== 调试开关 ==========
// 取消注释下面这行可以启用所有 debug 日志
// #define ENABLE_DEBUG
// ==================================

#include "galay/kernel/runtime/Runtime.h"
#include "galay-http/server/HttpsServer.h"
#include "galay-http/kernel/http/HttpsRouter.h"
#include "galay-http/kernel/http/HttpsWriter.h"
#include "galay-http/kernel/http2/Http2Connection.h"
#include "galay-http/kernel/http2/Http2Writer.h"
#include "galay-http/protoc/http2/Http2Hpack.h"
#include "galay-http/utils/HttpLogger.h"
#include "galay-http/utils/HttpsDebugLog.h"
#include "galay-http/utils/Http2DebugLog.h"
#include "galay-http/utils/HttpUtils.h"
#include <galay/utils/SignalHandler.hpp>
#include <csignal>
#include <fstream>
#include <iostream>
#include <map>

using namespace galay;
using namespace galay::http;

// 存储每个流的请求信息
struct StreamRequest {
    uint32_t stream_id;
    std::map<std::string, std::string> headers;
    std::string data;
    bool headers_complete = false;
    bool data_complete = false;
    
    std::string getMethod() const {
        auto it = headers.find(":method");
        return it != headers.end() ? it->second : "GET";
    }
    
    std::string getPath() const {
        auto it = headers.find(":path");
        return it != headers.end() ? it->second : "/";
    }
};

// 全局变量：存储所有流的请求信息
std::map<uint32_t, StreamRequest> g_stream_requests;

// HEADERS 帧回调
Coroutine<nil> onHeaders(Http2Connection& conn, 
                          uint32_t stream_id,
                          const std::map<std::string, std::string>& headers,
                          bool end_stream)
{
    HTTP2_LOG_INFO("========================================");
    HTTP2_LOG_INFO("[onHeaders] 📨 收到 HEADERS 帧 - stream={}, end_stream={}", stream_id, end_stream);
    
    // 存储请求头
    if (g_stream_requests.find(stream_id) == g_stream_requests.end()) {
        HTTP2_LOG_DEBUG("[onHeaders] 创建新的请求记录");
        g_stream_requests[stream_id] = StreamRequest{stream_id, headers, "", false, false};
    } else {
        HTTP2_LOG_DEBUG("[onHeaders] 更新已存在的请求记录");
        g_stream_requests[stream_id].headers = headers;
    }
    g_stream_requests[stream_id].headers_complete = true;
    
    // 打印关键头部
    HTTP2_LOG_DEBUG("[onHeaders] 请求头部：");
    for (const auto& [key, value] : headers) {
        if (key.starts_with(":")) {
            HTTP2_LOG_DEBUG("    {} = {}", key, value);
        }
    }
    
    // 如果是 END_STREAM，立即处理请求
    if (end_stream) {
        HTTP2_LOG_DEBUG("[onHeaders] end_stream=true，立即处理请求（GET 或无 body POST）");
        g_stream_requests[stream_id].data_complete = true;
        
        // 处理请求并发送响应
        auto& req = g_stream_requests[stream_id];
        std::string path = req.getPath();
        std::string method = req.getMethod();
        
        HTTP2_LOG_INFO("[HTTP/2] Request: {} {}", method, path);
        
        // 准备响应
        std::string response_body;
        std::string content_type;
        
        if (path == "/" || path == "/index.html") {
            content_type = "text/html; charset=utf-8";
            response_body = R"(<!DOCTYPE html>
<html>
<head>
    <title>HTTP/2 Test Server (h2)</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 40px; }
        h1 { color: #2196F3; }
        .info { background: #f0f0f0; padding: 15px; border-radius: 5px; }
        code { background: #e0e0e0; padding: 2px 5px; border-radius: 3px; }
    </style>
</head>
<body>
    <h1>HTTP/2 Test Server (h2)</h1>
    <div class="info">
        <p><strong>Protocol:</strong> HTTP/2 over TLS (ALPN)</p>
        <p><strong>Secure:</strong> Yes (HTTPS)</p>
        <p>This server uses ALPN to negotiate HTTP/2 connections over TLS.</p>
    </div>
    <h2>Available Endpoints:</h2>
    <ul>
        <li><code>/</code> - This page</li>
        <li><code>/api/hello</code> - JSON API endpoint</li>
        <li><code>/api/echo</code> - Echo POST data</li>
    </ul>
    <h2>Test with curl:</h2>
    <pre>curl -v --http2 https://localhost:8443/ --insecure
curl -v --http2 https://localhost:8443/api/hello --insecure
curl -v --http2 -d "Hello HTTP/2" https://localhost:8443/api/echo --insecure</pre>
</body>
</html>)";
        } else if (path == "/api/hello") {
            content_type = "application/json; charset=utf-8";
            response_body = R"({
    "message": "Hello from HTTP/2!",
    "protocol": "h2",
    "secure": true,
    "negotiation": "ALPN",
    "stream_id": )" + std::to_string(stream_id) + R"(
})";
        } else if (path == "/api/echo") {
            content_type = "application/json; charset=utf-8";
            response_body = R"({
    "message": "Echo endpoint",
    "method": ")" + method + R"(",
    "path": ")" + path + R"(",
    "data": ")" + req.data + R"(",
    "stream_id": )" + std::to_string(stream_id) + R"(
})";
        } else {
            content_type = "text/plain; charset=utf-8";
            response_body = "404 Not Found";
        }
        
        // 使用 HPACK 编码响应头
        HpackEncoder encoder;
        std::vector<HpackHeaderField> response_headers = {
            {":status", path.find("/api/") == std::string::npos && path != "/" && path != "/index.html" ? "404" : "200"},
            {"content-type", content_type},
            {"content-length", std::to_string(response_body.size())},
            {"server", "galay-http2/1.0"},
            {"x-stream-id", std::to_string(stream_id)}
        };
        std::string encoded_headers = encoder.encodeHeaders(response_headers);
        
        // 发送响应
        auto writer = conn.getWriter({});
        
        // 发送 HEADERS 帧
        auto headers_result = co_await writer.sendHeaders(stream_id, encoded_headers, false, true);
        if (!headers_result.has_value()) {
            HTTP2_LOG_ERROR("[HTTP/2] Failed to send HEADERS: {}", headers_result.error().message());
            co_return nil();
        }
        HTTP2_LOG_INFO("[HTTP/2] Sent HEADERS for stream {}", stream_id);
        
        // 发送 DATA 帧（带 END_STREAM）
        auto data_result = co_await writer.sendData(stream_id, response_body, true);
        if (!data_result.has_value()) {
            HTTP2_LOG_ERROR("[HTTP/2] Failed to send DATA: {}", data_result.error().message());
            co_return nil();
        }
        HTTP2_LOG_INFO("[HTTP/2] Sent DATA for stream {}, response complete", stream_id);
        
        // 清理请求信息
        g_stream_requests.erase(stream_id);
        HTTP2_LOG_DEBUG("[onHeaders] 请求处理完成并清理");
        HTTP2_LOG_INFO("========================================");
    } else {
        HTTP2_LOG_INFO("[onHeaders] ⏳ 等待 DATA 帧...");
        HTTP2_LOG_INFO("========================================");
    }
    
    co_return nil();
}

// DATA 帧回调
Coroutine<nil> onData(Http2Connection& conn,
                       uint32_t stream_id,
                       const std::string& data,
                       bool end_stream)
{
    HTTP2_LOG_INFO("========================================");
    HTTP2_LOG_INFO("[onData] 📦 收到 DATA 帧 - stream={}, size={} bytes, end_stream={}", 
                   stream_id, data.size(), end_stream);
    HTTP2_LOG_DEBUG("[onData] Data content: {}", data.substr(0, std::min<size_t>(100, data.size())));
    
    // 存储数据
    HTTP2_LOG_DEBUG("[onData] 查找 stream {} 的请求信息...", stream_id);
    if (g_stream_requests.find(stream_id) != g_stream_requests.end()) {
        HTTP2_LOG_DEBUG("[onData] 找到请求信息，累加数据");
        g_stream_requests[stream_id].data += data;
        
        if (end_stream) {
            HTTP2_LOG_DEBUG("[onData] end_stream=true，数据接收完成");
            g_stream_requests[stream_id].data_complete = true;
            
            // 如果头部已完成，处理请求
            HTTP2_LOG_DEBUG("[onData] 检查 headers_complete 标志...");
            if (g_stream_requests[stream_id].headers_complete) {
                HTTP2_LOG_DEBUG("[onData] headers_complete=true，开始处理完整请求");
                auto& req = g_stream_requests[stream_id];
                std::string path = req.getPath();
                std::string method = req.getMethod();
                
                HTTP2_LOG_INFO("[HTTP/2] Request complete: {} {}", method, path);
                
                // 准备响应
                std::string response_body = R"({
    "message": "Data received",
    "method": ")" + method + R"(",
    "path": ")" + path + R"(",
    "data_length": )" + std::to_string(req.data.size()) + R"(,
    "data": ")" + req.data + R"("
})";
                
                // 使用 HPACK 编码响应头
                HpackEncoder encoder;
                std::vector<HpackHeaderField> response_headers = {
                    {":status", "200"},
                    {"content-type", "application/json; charset=utf-8"},
                    {"content-length", std::to_string(response_body.size())},
                    {"server", "galay-http2/1.0"}
                };
                std::string encoded_headers = encoder.encodeHeaders(response_headers);
                
                // 发送响应
                auto writer = conn.getWriter({});
                
                auto headers_result = co_await writer.sendHeaders(stream_id, encoded_headers, false, true);
                if (headers_result.has_value()) {
                    auto data_result = co_await writer.sendData(stream_id, response_body, true);
                    if (data_result.has_value()) {
                        HTTP2_LOG_INFO("[HTTP/2] Response sent for stream {}", stream_id);
                    }
                }
                
                // 清理请求信息
                g_stream_requests.erase(stream_id);
                HTTP2_LOG_INFO("[onData] ✅ 处理完成: {} {}", method, path);
            } else {
                HTTP2_LOG_WARN("[onData] headers_complete=false，这不应该发生！");
            }
        } else {
            HTTP2_LOG_DEBUG("[onData] end_stream=false，继续等待更多数据");
        }
    } else {
        HTTP2_LOG_ERROR("[onData] ✗ 未找到 stream {} 的请求信息！", stream_id);
    }
    
    HTTP2_LOG_INFO("========================================");
    co_return nil();
}

// 错误回调
Coroutine<nil> onError(Http2Connection& conn, const Http2Error& error)
{
    HTTP2_LOG_ERROR("[HTTP/2] Error: {}", error.message());
    co_return nil();
}

// HTTP/1.1 降级处理器（当客户端不支持 HTTP/2 时）
Coroutine<nil> handleHttp1Index(HttpRequest& request, HttpsConnection& conn, HttpsParams params)
{
    HTTPS_LOG_INFO("[HTTP/1.1] Fallback request: {} {}", 
                   httpMethodToString(request.header().method()), 
                   request.header().uri());
    
    auto writer = conn.getResponseWriter({});
    
    std::string body = R"({
    "message": "This server supports HTTP/2",
    "current_protocol": "HTTP/1.1",
    "upgrade_hint": "Use curl with --http2 flag to access HTTP/2"
})";
    
    HttpResponse response;
    response.header().code() = HttpStatusCode::OK_200;
    response.header().version() = HttpVersion::Http_Version_1_1;
    response.header().headerPairs().addHeaderPair("Content-Type", "application/json; charset=utf-8");
    response.header().headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));
    response.setBodyStr(std::move(body));
    
    co_await writer.reply(response);
    co_await conn.close();
    
    co_return nil();
}

int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << "     HTTP/2 测试服务器 (h2)" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "监听地址: https://localhost:8443" << std::endl;
    std::cout << "协议: HTTP/2 over TLS (ALPN)" << std::endl;
    std::cout << "注意：需要 SSL 证书文件 server.crt 和 server.key" << std::endl;
    std::cout << "按 Ctrl+C 停止服务器" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    // 检查证书文件是否存在
    std::ifstream cert_file("server.crt");
    std::ifstream key_file("server.key");
    
    if (!cert_file.good() || !key_file.good()) {
        std::cerr << "错误：SSL 证书文件不存在！" << std::endl;
        std::cerr << std::endl;
        std::cerr << "请先生成自签名证书：" << std::endl;
        std::cerr << "openssl req -x509 -newkey rsa:4096 -keyout server.key -out server.crt -days 365 -nodes -subj \"/CN=localhost\"" << std::endl;
        std::cerr << std::endl;
        return 1;
    }
    
    // 设置日志级别 - 强制使用 debug 级别以便查看详细日志
    HttpLogger::getInstance()->getLogger()->getSpdlogger()->set_level(spdlog::level::level_enum::debug);
    HTTP2_LOG_DEBUG("========================================");
    HTTP2_LOG_DEBUG("日志级别: DEBUG (显示所有详细日志)");
    HTTP2_LOG_DEBUG("========================================");
    
    // 创建运行时
    RuntimeBuilder runtimebuilder;
    auto runtime = runtimebuilder.build();
    runtime.start();
    
    // 创建 HTTP/1.1 降级路由
    HttpsRouter http1_router;
    HttpsRouteMap routes = {
        {"/", handleHttp1Index}
    };
    http1_router.addRoute<GET>(routes);
    
    // 创建 HTTP/2 回调
    Http2Callbacks http2_callbacks;
    http2_callbacks.on_headers = onHeaders;
    http2_callbacks.on_data = onData;
    http2_callbacks.on_error = onError;
    
    // 验证回调是否设置成功
    if (!http2_callbacks.on_headers) {
        HTTP2_LOG_ERROR("Failed to set on_headers callback");
        return 1;
    }
    HTTP2_LOG_INFO("HTTP/2 callbacks configured successfully");
    
    // 创建 HTTPS 服务器（启用 HTTP/2）
    HttpsServer server = HttpsServerBuilder("server.crt", "server.key")
                            .addListen(Host("0.0.0.0", 8443))
                            .enableHttp2(true)  // 启用 HTTP/2
                            .build();
    
    // 设置信号处理
    utils::SignalHandler::setSignalHandler<SIGINT>([&server](int signal) {
        HTTP2_LOG_INFO("接收到停止信号 ({}), 关闭服务器", signal);
        server.stop();
    });
    
    std::cout << "服务器启动成功！" << std::endl;
    std::cout << std::endl;
    std::cout << "测试命令：" << std::endl;
    std::cout << "  # 测试主页" << std::endl;
    std::cout << "  curl -v --http2 https://localhost:8443/ --insecure" << std::endl;
    std::cout << std::endl;
    std::cout << "  # 测试 API" << std::endl;
    std::cout << "  curl -v --http2 https://localhost:8443/api/hello --insecure" << std::endl;
    std::cout << std::endl;
    std::cout << "  # 测试 POST" << std::endl;
    std::cout << "  curl -v --http2 -d 'Hello HTTP/2' https://localhost:8443/api/echo --insecure" << std::endl;
    std::cout << std::endl;
    std::cout << "注意：浏览器访问时会显示证书警告（因为是自签名证书），这是正常的。" << std::endl;
    std::cout << "========================================" << std::endl;
    
    HTTP2_LOG_INFO("Starting server with HTTP/2 support...");
    
    // 运行服务器（自动检测 HTTP/2）
    server.run(runtime, http1_router, http2_callbacks);
    server.wait();
    
    HTTP2_LOG_INFO("服务器已停止");
    return 0;
}

