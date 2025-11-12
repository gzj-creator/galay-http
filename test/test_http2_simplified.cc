// HTTP/2 简化接口示例
// 
// 功能：展示如何使用高度封装的 Http2StreamHelper，不需要理解帧操作
//
// 编译:
//   cd build && cmake .. && make test_http2_simplified
//
// 运行:
//   cd build/test && ./test_http2_simplified
//
// 测试:
//   curl --http2 https://localhost:8443/api/hello --insecure
//   curl --http2 https://localhost:8443/static/yourfile.txt --insecure

#include "galay/kernel/runtime/Runtime.h"
#include "galay-http/server/Http2Server.h"
#include "galay-http/kernel/http2/Http2StreamHelper.h"
#include "galay-http/utils/HttpLogger.h"
#include "galay/kernel/coroutine/AsyncWaiter.hpp"
#include <galay/utils/SignalHandler.hpp>
#include <iostream>
#include <iomanip>

using namespace galay;
using namespace galay::http;
using enum galay::http::HttpStatusCode;

// 静态文件目录
const std::string STATIC_DIR = "/Users/gongzhijie/Desktop/zhongxin";

// ==================== HTTP/2 请求处理 ====================

Coroutine<nil> onHeaders(Http2Connection& conn,
                          uint32_t stream_id,
                          const std::map<std::string, std::string>& headers,
                          bool end_stream)
{
    // 提取请求信息
    std::string method, path;
    for (const auto& [key, value] : headers) {
        if (key == ":method") method = value;
        else if (key == ":path") path = value;
    }
    
    std::cout << "[HTTP/2] " << method << " " << path << std::endl;
    
    // 创建流辅助对象（高度封装，不需要理解帧操作）
    Http2StreamHelper helper(conn, stream_id);
    
    // ==================== API 路由 ====================
    
    if (path == "/api/hello") {
        // 发送 JSON 响应
        AsyncWaiter<void, Http2Error> waiter;
        auto co = helper.sendJson(OK_200, R"({"message": "Hello HTTP/2!", "protocol": "h2"})");
        co.then([&waiter](){ waiter.notify({}); });
        waiter.appendTask(std::move(co));
        co_await waiter.wait();
        co_return nil();
    }
    
    if (path == "/api/text") {
        // 发送文本响应
        AsyncWaiter<void, Http2Error> waiter;
        auto co = helper.sendText(OK_200, "Hello from HTTP/2!");
        co.then([&waiter](){ waiter.notify({}); });
        waiter.appendTask(std::move(co));
        co_await waiter.wait();
        co_return nil();
    }
    
    if (path == "/api/html") {
        // 发送 HTML 响应
        AsyncWaiter<void, Http2Error> waiter;
        auto co = helper.sendHtml(OK_200, "<h1>Hello HTTP/2</h1><p>This is a test page.</p>");
        co.then([&waiter](){ waiter.notify({}); });
        waiter.appendTask(std::move(co));
        co_await waiter.wait();
        co_return nil();
    }
    
    // ==================== 静态文件服务 ====================
    
    if (path.starts_with("/static/")) {
        // 使用静态文件服务器（自动处理文件发送、分片、流控）
        AsyncWaiter<void, Http2Error> waiter;
        auto co = Http2StaticFileServer::serve(
            conn, stream_id,
            "/static",              // URL 前缀
            STATIC_DIR,             // 本地目录
            path,                   // 请求路径
            [](const std::string& file_path, size_t sent, size_t total) {
                // 进度回调
                double progress = (sent * 100.0) / total;
                static std::map<std::string, int> last_progress;
                int step = static_cast<int>(progress / 10);
                
                if (last_progress[file_path] != step || sent == total) {
                    last_progress[file_path] = step;
                    std::cout << "📊 " << file_path << ": "
                             << std::fixed << std::setprecision(1) << progress << "% "
                             << "(" << sent / 1024.0 / 1024.0 << " MB / "
                             << total / 1024.0 / 1024.0 << " MB)" << std::endl;
                    
                    if (sent == total) {
                        last_progress.erase(file_path);
                    }
                }
            }
        );
        co.then([&waiter](){ waiter.notify({}); });
        waiter.appendTask(std::move(co));
        auto result = co_await waiter.wait();
        
        if (!result.has_value()) {
            std::cout << "Failed to serve file: " << path << std::endl;
        }
        
        co_return nil();
    }
    
    // ==================== 404 处理 ====================
    
    // 发送错误响应
    AsyncWaiter<void, Http2Error> waiter;
    auto co = helper.sendError(NotFound_404);
    co.then([&waiter](){ waiter.notify({}); });
    waiter.appendTask(std::move(co));
    co_await waiter.wait();
    co_return nil();
}

Coroutine<nil> onError(Http2Connection& conn, const Http2Error& error)
{
    std::cerr << "[HTTP/2] Error: " << error.message() << std::endl;
    co_return nil();
}

// ==================== 主函数 ====================

int main()
{
    std::cout << "========================================\n";
    std::cout << "  HTTP/2 简化接口示例\n";
    std::cout << "========================================\n";
    std::cout << "监听地址: https://localhost:8443\n";
    std::cout << "协议: HTTP/2 (h2)\n";
    std::cout << "========================================\n\n";
    
    // 设置日志级别
    HttpLogger::getInstance()->getLogger()->getSpdlogger()->set_level(spdlog::level::level_enum::info);
    
    // 创建运行时
    RuntimeBuilder runtimebuilder;
    auto runtime = runtimebuilder.build();
    runtime.start();
    
    // 配置 HTTP/2 回调
    Http2Callbacks callbacks;
    callbacks.on_headers = onHeaders;
    callbacks.on_error = onError;
    
    // 创建 HTTP/2 服务器
    Http2Server server = Http2ServerBuilder("server.crt", "server.key")
                            .addListen(Host("0.0.0.0", 8443))
                            .build();
    
    // 信号处理
    utils::SignalHandler::setSignalHandler<SIGINT>([&server](int signal) {
        std::cout << "\n接收到停止信号，关闭服务器...\n";
        server.stop();
    });
    
    std::cout << "✅ 服务器启动成功！\n\n";
    std::cout << "可用端点：\n";
    std::cout << "  /api/hello      - JSON 响应\n";
    std::cout << "  /api/text       - 文本响应\n";
    std::cout << "  /api/html       - HTML 响应\n";
    std::cout << "  /static/*       - 静态文件\n\n";
    std::cout << "测试命令：\n";
    std::cout << "  curl --http2 https://localhost:8443/api/hello --insecure\n";
    std::cout << "  curl --http2 https://localhost:8443/static/yourfile.txt --insecure\n\n";
    std::cout << "特性：\n";
    std::cout << "  ✓ 高度封装的接口（sendFile, sendJson, sendHtml, sendError）\n";
    std::cout << "  ✓ 不需要理解帧、HPACK 等底层细节\n";
    std::cout << "  ✓ 自动处理分片和流控\n";
    std::cout << "  ✓ 内置静态文件服务器\n";
    std::cout << "  ✓ 实时进度监控\n";
    std::cout << "========================================\n";
    
    // 启动服务器
    server.run(runtime, callbacks);
    server.wait();
    
    std::cout << "服务器已停止\n";
    return 0;
}

