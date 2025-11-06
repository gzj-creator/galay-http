// HTTP/2 over TLS (h2) 测试服务器
// 
// 这个示例展示了如何使用 Http2Server 提供 HTTP/2 over TLS 服务
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

#include "galay/common/Log.h"
#include "galay/kernel/runtime/Runtime.h"
#include "galay-http/server/Http2Server.h"
#include "galay-http/kernel/http/HttpsRouter.h"
#include "galay-http/kernel/http/HttpsWriter.h"
#include "galay-http/kernel/http2/Http2Connection.h"
#include "galay-http/kernel/http2/Http2Writer.h"
#include "galay-http/protoc/http2/Http2Hpack.h"
#include "galay-http/utils/HttpLogger.h"
#include "galay-http/utils/Http2DebugLog.h"
#include <galay/utils/SignalHandler.hpp>
#include <csignal>
#include <fstream>
#include <sstream>
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
        
        // 处理 OPTIONS 预检请求
        if (method == "OPTIONS") {
            HTTP2_LOG_INFO("[HTTP/2] Handling OPTIONS preflight request");
            
            HpackEncoder encoder;
            std::vector<HpackHeaderField> options_headers = {
                {":status", "204"},
                {"access-control-allow-origin", "*"},
                {"access-control-allow-methods", "GET, POST, OPTIONS"},
                {"access-control-allow-headers", "Content-Type, X-Request-ID, X-Timestamp, X-Custom-Header-1, X-Custom-Header-2, X-Custom-Header-3, X-Custom-Header-4, X-Custom-Header-5, User-Agent, Accept, Accept-Language, Accept-Encoding"},
                {"access-control-expose-headers", "X-Protocol, X-Stream-Id"},
                {"access-control-max-age", "86400"},
                {"content-length", "0"}
            };
            std::string encoded_headers = encoder.encodeHeaders(options_headers);
            
            auto writer = conn.getWriter({});
            auto headers_result = co_await writer.sendHeaders(stream_id, encoded_headers, true, true);
            if (!headers_result.has_value()) {
                HTTP2_LOG_ERROR("[HTTP/2] Failed to send OPTIONS response: {}", headers_result.error().message());
            } else {
                HTTP2_LOG_INFO("[HTTP/2] OPTIONS response sent for stream {}", stream_id);
            }
            
            // 删除流以释放资源
            conn.streamManager().removeStream(stream_id);
            HTTP2_LOG_DEBUG("[onHeaders] Stream {} removed from manager", stream_id);
            
            g_stream_requests.erase(stream_id);
            HTTP2_LOG_INFO("========================================");
            co_return nil();
        }
        
        // 准备响应
        std::string response_body;
        std::string content_type;
        std::string status_code = "200";
        
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
        <li><code>/test</code> - <a href="/test" style="color: #2196F3; font-weight: bold;">HTTP/2 Interactive Test Page</a></li>
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
        } else if (path == "/test" || path == "/test.html" || path == "/test_h2.html") {
            // 读取测试页面
            content_type = "text/html; charset=utf-8";
            std::ifstream file("../../test/html/test_h2.html");
            if (file.is_open()) {
                std::stringstream buffer;
                buffer << file.rdbuf();
                response_body = buffer.str();
                file.close();
                HTTP2_LOG_INFO("[HTTP/2] Serving test_h2.html ({} bytes)", response_body.size());
            } else {
                response_body = "Error: test_h2.html not found";
                HTTP2_LOG_ERROR("[HTTP/2] Failed to open ../../test/html/test_h2.html");
            }
        } else {
            content_type = "text/plain; charset=utf-8";
            response_body = "404 Not Found";
            status_code = "404";
        }
        
        // 使用 HPACK 编码响应头
        HpackEncoder encoder;
        std::vector<HpackHeaderField> response_headers = {
            {":status", status_code},
            {"content-type", content_type},
            {"content-length", std::to_string(response_body.size())},
            {"server", "galay-http2/1.0"},
            {"x-stream-id", std::to_string(stream_id)},
            {"x-protocol", "h2"},
            {"access-control-allow-origin", "*"},
            {"access-control-allow-methods", "GET, POST, OPTIONS"},
            {"access-control-allow-headers", "Content-Type, X-Request-ID, X-Timestamp, X-Custom-Header-1, X-Custom-Header-2, X-Custom-Header-3, X-Custom-Header-4, X-Custom-Header-5, User-Agent, Accept, Accept-Language, Accept-Encoding"},
            {"access-control-expose-headers", "X-Protocol, X-Stream-Id"}
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
        
        // 删除流以释放资源
        conn.streamManager().removeStream(stream_id);
        HTTP2_LOG_DEBUG("[onHeaders] Stream {} removed from manager", stream_id);
        
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
                    {"server", "galay-http2/1.0"},
                    {"x-stream-id", std::to_string(stream_id)},
                    {"x-protocol", "h2"},
                    {"access-control-allow-origin", "*"},
                    {"access-control-allow-methods", "GET, POST, OPTIONS"},
                    {"access-control-allow-headers", "Content-Type, X-Request-ID, X-Timestamp, X-Custom-Header-1, X-Custom-Header-2, X-Custom-Header-3, X-Custom-Header-4, X-Custom-Header-5, User-Agent, Accept, Accept-Language, Accept-Encoding"},
                    {"access-control-expose-headers", "X-Protocol, X-Stream-Id"}
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
                
                // 删除流以释放资源
                conn.streamManager().removeStream(stream_id);
                HTTP2_LOG_DEBUG("[onData] Stream {} removed from manager", stream_id);
                
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

// HTTP/1.1 降级处理器
Coroutine<nil> handleHttp1Index(HttpRequest& request, HttpsConnection& conn, HttpsParams params)
{
    HTTP2_LOG_INFO("[HTTP/1.1 Fallback] {} {}", 
                   httpMethodToString(request.header().method()), 
                   request.header().uri());
    
    auto writer = conn.getResponseWriter({});
    
    std::string path = request.header().uri();
    std::string body;
    std::string content_type;
    HttpStatusCode status = HttpStatusCode::OK_200;
    LogInfo("path: {}", path);
    if (path == "/" || path == "/index.html") {
        content_type = "text/html; charset=utf-8";
        body = R"(<!DOCTYPE html>
<html>
<head>
    <title>HTTP/1.1 Fallback</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 40px; background: #f0f0f0; }
        .container { background: white; padding: 30px; border-radius: 10px; max-width: 800px; margin: 0 auto; }
        h1 { color: #ff6b6b; }
        .info { background: #fff3cd; padding: 15px; border-radius: 5px; margin: 20px 0; }
    </style>
</head>
<body>
    <div class="container">
        <h1>⚠️ HTTP/1.1 降级模式</h1>
        <div class="info">
            <p><strong>当前协议:</strong> HTTP/1.1</p>
            <p><strong>说明:</strong> 你的浏览器不支持 HTTP/2 或 ALPN 协商失败，服务器已自动降级到 HTTP/1.1</p>
        </div>
        <h2>可用端点：</h2>
        <ul>
            <li><code>/</code> - 此页面</li>
            <li><code>/test</code> - <a href="/test">HTTP/2 测试页面</a>（需要 HTTP/2 支持）</li>
            <li><code>/api/hello</code> - JSON API</li>
        </ul>
        <h2>建议：</h2>
        <p>请使用支持 HTTP/2 的现代浏览器访问：</p>
        <ul>
            <li>Chrome 49+</li>
            <li>Firefox 52+</li>
            <li>Safari 10+</li>
            <li>Edge 79+</li>
        </ul>
    </div>
</body>
</html>)";
    } else if (path == "/test" || path == "/test.html" || path == "/test_h2.html") {
        // 读取 HTTP/2 测试页面
        content_type = "text/html; charset=utf-8";
        std::ifstream file("../../test/html/test_h2.html");
        if (file.is_open()) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            body = buffer.str();
            file.close();
            HTTP2_LOG_INFO("[HTTP/1.1 Fallback] Serving test_h2.html ({} bytes)", body.size());
        } else {
            body = "Error: test_h2.html not found";
            status = HttpStatusCode::NotFound_404;
            HTTP2_LOG_ERROR("[HTTP/1.1 Fallback] Failed to open ../../test/html/test_h2.html");
        }
    } else if (path == "/fallback" || path == "/test_http1_fallback.html") {
        // 读取 HTTP/1.1 fallback 测试页面
        content_type = "text/html; charset=utf-8";
        std::ifstream file("../../test/html/test_http1_fallback.html");
        if (file.is_open()) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            body = buffer.str();
            file.close();
            HTTP2_LOG_INFO("[HTTP/1.1 Fallback] Serving test_http1_fallback.html ({} bytes)", body.size());
        } else {
            body = "Error: test_http1_fallback.html not found";
            status = HttpStatusCode::NotFound_404;
            HTTP2_LOG_ERROR("[HTTP/1.1 Fallback] Failed to open ../../test/html/test_http1_fallback.html");
        }
    } else if (path == "/api/hello") {
        content_type = "application/json; charset=utf-8";
        body = R"({
    "message": "Hello from HTTP/1.1!",
    "protocol": "http/1.1",
    "secure": true,
    "note": "Fallback mode - HTTP/2 not available"
})";
    } else {
        content_type = "text/plain; charset=utf-8";
        body = "404 Not Found";
        status = HttpStatusCode::NotFound_404;
    }
    
    HttpResponse response;
    response.header().code() = status;
    response.header().version() = HttpVersion::Http_Version_1_1;
    response.header().headerPairs().addHeaderPair("Content-Type", content_type);
    response.header().headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));
    response.header().headerPairs().addHeaderPair("Server", "galay-http2/1.0");
    response.header().headerPairs().addHeaderPair("Access-Control-Allow-Origin", "*");
    response.setBodyStr(std::move(body));
    
    co_await writer.reply(response);
    
    if (request.header().isConnectionClose()) {
        co_await conn.close();
    }
    
    co_return nil();
}

int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << "     HTTP/2 测试服务器 (h2)" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "监听地址: https://localhost:8443" << std::endl;
    std::cout << "协议: HTTP/2 over TLS (h2) + HTTP/1.1 降级" << std::endl;
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
    //HttpLogger::getInstance()->getLogger()->getSpdlogger()->set_level(spdlog::level::level_enum::debug);
    HTTP2_LOG_DEBUG("========================================");
    HTTP2_LOG_DEBUG("日志级别: DEBUG (显示所有详细日志)");
    HTTP2_LOG_DEBUG("========================================");
    
    // 创建运行时
    RuntimeBuilder runtimebuilder;
    auto runtime = runtimebuilder.build();
    runtime.start();
    
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
    
    // 创建 HTTP/1.1 降级路由
    HttpsRouter http1_router;
    HttpsRouteMap routes = {
        {"/", handleHttp1Index},
        {"/test", handleHttp1Index},
        {"/test.html", handleHttp1Index},
        {"/test_h2.html", handleHttp1Index},
        {"/fallback", handleHttp1Index},
        {"/test_http1_fallback.html", handleHttp1Index},
        {"/api/hello", handleHttp1Index},
        {"/api/echo", handleHttp1Index}
    };
    http1_router.addRoute<GET>(routes);
    http1_router.addRoute<POST>(routes);
    HTTP2_LOG_INFO("HTTP/1.1 fallback router configured");
    
    // 创建 HTTP/2 服务器（支持 h2 + http/1.1 降级）
    Http2Server server = Http2ServerBuilder("server.crt", "server.key")
                            .addListen(Host("0.0.0.0", 8443))
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
    std::cout << "  # 浏览器测试页面" << std::endl;
    std::cout << "  https://localhost:8443/test" << std::endl;
    std::cout << std::endl;
    std::cout << "注意：" << std::endl;
    std::cout << "  - 支持 HTTP/2 的客户端会使用 h2 协议" << std::endl;
    std::cout << "  - 不支持 HTTP/2 的客户端会自动降级到 HTTP/1.1" << std::endl;
    std::cout << "  - 浏览器访问时会显示证书警告（因为是自签名证书），这是正常的" << std::endl;
    std::cout << "  - ALPN 配置: h2, http/1.1 (h2 优先)" << std::endl;
    std::cout << "========================================" << std::endl;
    
    HTTP2_LOG_INFO("Starting HTTP/2 server with HTTP/1.1 fallback...");
    
    // 运行服务器（支持降级）
    server.run(runtime, http2_callbacks, http1_router);
    server.wait();
    
    HTTP2_LOG_INFO("服务器已停止");
    return 0;
}

