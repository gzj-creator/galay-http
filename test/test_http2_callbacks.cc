// HTTP/2 回调系统测试程序
// 
// 这个示例展示了如何使用 HttpsServer 的回调系统来处理 HTTP/2 请求
// 
// 编译:
//   cd build && make test_http2_callbacks
// 
// 运行:
//   cd build/test && ./test_http2_callbacks
// 
// 测试:
//   curl -v --http2 https://localhost:8443/ --insecure
//   curl -v --http2 https://localhost:8443/api/hello --insecure

#include <galay/common/Common.h>
#include <galay/kernel/runtime/Runtime.h>
#include <galay/utils/SignalHandler.hpp>
#include "galay-http/server/HttpsServer.h"
#include "galay-http/kernel/http/HttpsRouter.h"
#include "galay-http/kernel/http/HttpsWriter.h"
#include "galay-http/kernel/http2/Http2Connection.h"
#include "galay-http/kernel/http2/Http2Writer.h"
#include "galay-http/protoc/http2/Http2Hpack.h"
#include "galay-http/protoc/alpn/AlpnProtocol.h"
#include "galay-http/utils/HttpLogger.h"
#include "galay-http/utils/HttpDebugLog.h"
#include "galay-http/utils/Http2DebugLog.h"
#include <csignal>
#include <fstream>
#include <iostream>
#include <atomic>
#include <map>

using namespace galay;
using namespace galay::http;

std::atomic<bool> g_stop_flag{false};

// 用于存储每个流的请求信息
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
    
    std::string getScheme() const {
        auto it = headers.find(":scheme");
        return it != headers.end() ? it->second : "https";
    }
    
    std::string getAuthority() const {
        auto it = headers.find(":authority");
        return it != headers.end() ? it->second : "";
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
    HTTP2_LOG_INFO("[Callback] Received HEADERS on stream {}, end_stream={}", stream_id, end_stream);
    
    // 存储请求头
    if (g_stream_requests.find(stream_id) == g_stream_requests.end()) {
        g_stream_requests[stream_id] = StreamRequest{stream_id, headers, "", false, false};
    } else {
        g_stream_requests[stream_id].headers = headers;
    }
    g_stream_requests[stream_id].headers_complete = true;
    
    // 打印关键头部
    for (const auto& [key, value] : headers) {
        HTTP2_LOG_DEBUG("[Callback]   {} = {}", key, value);
    }
    
    // 如果是 END_STREAM，立即处理请求
    if (end_stream) {
        g_stream_requests[stream_id].data_complete = true;
        
        // 处理请求并发送响应
        auto& req = g_stream_requests[stream_id];
        std::string path = req.getPath();
        std::string method = req.getMethod();
        
        HTTP2_LOG_INFO("[Callback] Processing request: {} {}", method, path);
        
        // 构造响应
        std::string response_body;
        std::map<std::string, std::string> response_headers = {
            {":status", "200"},
            {"content-type", "application/json"},
            {"server", "galay-http-h2"}
        };
        
        if (path == "/") {
            response_body = R"({
    "message": "Welcome to HTTP/2 server with callbacks!",
    "protocol": "HTTP/2",
    "stream_id": )" + std::to_string(stream_id) + R"(,
    "method": ")" + method + R"(",
    "path": ")" + path + R"("
})";
        } else if (path == "/api/hello") {
            response_body = R"({
    "message": "Hello from HTTP/2!",
    "stream_id": )" + std::to_string(stream_id) + R"(
})";
        } else {
            response_headers[":status"] = "404";
            response_body = R"({
    "error": "Not Found",
    "path": ")" + path + R"("
})";
        }
        
        response_headers["content-length"] = std::to_string(response_body.size());
        
        // 发送 HEADERS 帧
        auto writer = conn.getWriter({});
        HpackEncoder encoder;
        // 转换 map 到 vector<HpackHeaderField>
        std::vector<HpackHeaderField> header_fields;
        for (const auto& [name, value] : response_headers) {
            header_fields.push_back({name, value});
        }
        std::string encoded_headers = encoder.encodeHeaders(header_fields);
        
        auto send_headers_res = co_await writer.sendHeaders(stream_id, encoded_headers, false);
        if (!send_headers_res) {
            HTTP2_LOG_ERROR("[Callback] Failed to send HEADERS: {}", send_headers_res.error().message());
            co_return nil();
        }
        
        // 发送 DATA 帧
        auto send_data_res = co_await writer.sendData(stream_id, response_body, true);
        if (!send_data_res) {
            HTTP2_LOG_ERROR("[Callback] Failed to send DATA: {}", send_data_res.error().message());
            co_return nil();
        }
        
        HTTP2_LOG_INFO("[Callback] Response sent on stream {}", stream_id);
        
        // 清理请求信息
        g_stream_requests.erase(stream_id);
    }
    
    co_return nil();
}

// DATA 帧回调
Coroutine<nil> onData(Http2Connection& conn,
                       uint32_t stream_id,
                       const std::string& data,
                       bool end_stream)
{
    HTTP2_LOG_INFO("[Callback] Received DATA on stream {}, length={}, end_stream={}", 
                  stream_id, data.size(), end_stream);
    
    // 累积数据
    if (g_stream_requests.find(stream_id) != g_stream_requests.end()) {
        g_stream_requests[stream_id].data += data;
        
        if (end_stream) {
            g_stream_requests[stream_id].data_complete = true;
            HTTP2_LOG_DEBUG("[Callback] Complete request body: {}", g_stream_requests[stream_id].data);
            
            // TODO: 这里可以处理 POST/PUT 请求的 body
        }
    }
    
    co_return nil();
}

// 错误回调
Coroutine<nil> onError(Http2Connection& conn, const Http2Error& error)
{
    HTTP2_LOG_ERROR("[Callback] HTTP/2 error: {}", error.message());
    co_return nil();
}

// HTTP/1.1 路由处理器
Coroutine<nil> handleHttp1Index(HttpRequest& request, HttpsConnection& conn, HttpsParams params)
{
    HTTP_LOG_INFO("[HTTP/1.1] GET /");
    
    std::string body = R"({
    "message": "This is HTTP/1.1 endpoint",
    "upgrade_to_http2": "Use --http2 with curl to access HTTP/2"
})";
    
    auto writer = conn.getResponseWriter({});
    HttpResponse response;
    response.header().code() = HttpStatusCode::OK_200;
    response.header().headerPairs().addHeaderPair("Content-Type", "application/json");
    response.header().headerPairs().addHeaderPair("Server", "galay-http");
    response.setBodyStr(std::move(body));
    
    co_await writer.reply(response);
    co_return nil();
}

int main()
{
    // 生成自签名证书（如果不存在）
    if (!std::ifstream("server.crt") || !std::ifstream("server.key")) {
        std::cout << "Generating self-signed SSL certificate and key..." << std::endl;
        std::string cmd = "openssl req -x509 -newkey rsa:4096 -keyout server.key -out server.crt -days 365 -nodes -subj '/CN=localhost'";
        system(cmd.c_str());
    }

    HttpLogger::getInstance()->getLogger()->getSpdlogger()->set_level(spdlog::level::level_enum::info);
    HTTP_LOG_INFO("[Main] Log level set to INFO");

    // 注意：TcpSslServer 内部会自动初始化 SSL_CTX
    // 不需要手动调用 initializeSSLServerEnv()
    HTTP_LOG_INFO("[Main] SSL will be initialized automatically by framework");
    
    // 创建运行时
    RuntimeBuilder runtimebuilder;
    auto runtime = runtimebuilder.build();
    runtime.start();

    // 创建 HTTP/1.1 路由
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
    
    // 验证回调
    if (!http2_callbacks.isValid()) {
        HTTP_LOG_ERROR("[Main] HTTP/2 callbacks are not valid (missing on_headers or on_data)");
        return 1;
    }

    // 创建 HTTPS 服务器
    HttpsServer server = HttpsServerBuilder("server.crt", "server.key")
                            .enableHttp2(true)
                            .addListen(Host("0.0.0.0", 8443))
                            .build();
    
    // ⚠️ 重要：必须显式调用 listen() 来真正监听端口！
    // HttpsServerBuilder::addListen() 只是配置，不会实际监听
    server.listen(Host("0.0.0.0", 8443));

    utils::SignalHandler::setSignalHandler<SIGINT>([&server](int signal) {
        HTTP_LOG_INFO("\n[Main] Received signal: {}, shutting down...", signal);
        g_stop_flag = true;
        server.stop();
    });
    
    // 打印服务器信息
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << "  🚀 HTTP/2 Callback System Test\n";
    std::cout << "========================================\n";
    std::cout << "Listening on: https://localhost:8443\n";
    std::cout << "\n";
    std::cout << "Protocol Support:\n";
    std::cout << "  ✅ HTTP/1.1 over TLS (with router)\n";
    std::cout << "  ✅ HTTP/2 over TLS (with callbacks)\n";
    std::cout << "\n";
    std::cout << "HTTP/2 Features:\n";
    std::cout << "  • Automatic frame dispatch\n";
    std::cout << "  • User-defined callbacks\n";
    std::cout << "  • Auto SETTINGS/PING ACK\n";
    std::cout << "  • Stream management\n";
    std::cout << "\n";
    std::cout << "Quick Tests:\n";
    std::cout << "  🌐 HTTP/1.1:\n";
    std::cout << "     curl -v --http1.1 https://localhost:8443/ --insecure\n";
    std::cout << "\n";
    std::cout << "  🚄 HTTP/2:\n";
    std::cout << "     curl -v --http2 https://localhost:8443/ --insecure\n";
    std::cout << "     curl -v --http2 https://localhost:8443/api/hello --insecure\n";
    std::cout << "\n";
    std::cout << "Press Ctrl+C to stop\n";
    std::cout << "========================================\n";
    std::cout << "\n";
    
    HTTP_LOG_INFO("[Main] Server starting with callback system...");
    
    // 添加调试日志
    std::cout << "[DEBUG] Before server.run()" << std::endl;
    std::cout << "[DEBUG] Runtime address: " << &runtime << std::endl;
    std::cout << "[DEBUG] Router address: " << &http1_router << std::endl;
    std::cout << "[DEBUG] Callbacks valid: " << http2_callbacks.isValid() << std::endl;
    
    // 运行服务器（使用回调）
    server.run(runtime, http1_router, http2_callbacks);
    
    std::cout << "[DEBUG] After server.run()" << std::endl;
    std::cout << "[DEBUG] Calling server.wait()..." << std::endl;
    
    server.wait();
    
    std::cout << "[DEBUG] server.wait() returned" << std::endl;
    
    // 注意：不需要手动调用 destroySSLEnv()，因为我们没有手动初始化
    HTTP_LOG_INFO("[Main] Server stopped");
    
    return 0;
}

