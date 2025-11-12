// HTTP/2 路由器 + HTTP/1.1 降级示例
// 
// 功能：演示如何使用 Http2Router 和 HttpsRouter 实现完美的降级支持
//
// 编译:
//   cd build && cmake .. && make test_http2_router_with_fallback
//
// 运行:
//   cd build/test && ./test_http2_router_with_fallback
//
// 测试:
//   # HTTP/2 测试
//   curl --http2 https://localhost:8443/static/test.html --insecure
//   
//   # HTTP/1.1 降级测试
//   curl --http1.1 https://localhost:8443/static/test.html --insecure

#include "galay/kernel/runtime/Runtime.h"
#include "galay-http/server/Http2Server.h"
#include "galay-http/kernel/http2/Http2Router.h"
#include "galay-http/kernel/http/HttpsRouter.h"
#include "galay-http/kernel/http/HttpParams.hpp"
#include "galay-http/utils/HttpLogger.h"
#include <galay/utils/SignalHandler.hpp>
#include <iostream>
#include <iomanip>

using namespace galay;
using namespace galay::http;

int main()
{
    std::cout << "========================================\n";
    std::cout << "  HTTP/2 + HTTP/1.1 路由器示例\n";
    std::cout << "========================================\n";
    std::cout << "监听地址: https://localhost:8443\n";
    std::cout << "协议: HTTP/2 (h2) + HTTP/1.1 fallback\n";
    std::cout << "========================================\n\n";
    
    // 设置日志级别
    HttpLogger::getInstance()->getLogger()->getSpdlogger()->set_level(spdlog::level::level_enum::info);
    
    // 创建运行时
    RuntimeBuilder runtimebuilder;
    auto runtime = runtimebuilder.build();
    runtime.start();
    
    // ========================================
    // 核心部分：创建路由器并挂载静态文件
    // 就像 HTTP/1.1 一样简单！
    // ========================================
    
    std::string static_dir = "/Users/gongzhijie/Desktop/zhongxin";
    
    // 1. HTTP/2 路由器
    Http2Router http2Router;
    http2Router.mount("/static", static_dir,
        [](uint32_t stream_id, const std::string& path, size_t sent, size_t total) {
            // HTTP/2 进度回调
            double progress = (sent * 100.0) / total;
            static std::map<uint32_t, int> last_progress;
            int step = static_cast<int>(progress / 10);
            
            if (last_progress[stream_id] != step || sent == total) {
                last_progress[stream_id] = step;
                std::cout << "[HTTP/2] 📊 Stream " << stream_id << ": " 
                         << std::fixed << std::setprecision(1) << progress << "% "
                         << "(" << sent / 1024.0 / 1024.0 << " MB / " 
                         << total / 1024.0 / 1024.0 << " MB)" << std::endl;
                
                if (sent == total) {
                    last_progress.erase(stream_id);
                }
            }
        }
    );
    
    // 2. HTTP/1.1 路由器（降级时使用）
    HttpsRouter http1Router;
    http1Router.mount("/static", static_dir,
        [](const HttpRequest& req, size_t sent, size_t total, const FileTransferInfo& info) {
            // HTTP/1.1 进度回调
            double progress = (sent * 100.0) / total;
            static std::map<std::string, int> last_progress;
            int step = static_cast<int>(progress / 10);
            
            if (last_progress[info.relative_path] != step || sent == total) {
                last_progress[info.relative_path] = step;
                std::cout << "[HTTP/1.1] 📊 " << info.relative_path << ": " 
                         << std::fixed << std::setprecision(1) << progress << "% "
                         << "(" << sent / 1024.0 / 1024.0 << " MB / " 
                         << total / 1024.0 / 1024.0 << " MB)" << std::endl;
                
                if (sent == total) {
                    last_progress.erase(info.relative_path);
                }
            }
        },
        {
            .use_sendfile = true,      // HTTP/1.1 使用 sendfile 零拷贝
            .support_range = true       // 支持断点续传
        }
    );
    
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
    std::cout << "📁 静态文件目录: " << static_dir << "\n\n";
    std::cout << "测试命令：\n";
    std::cout << "  # HTTP/2 测试\n";
    std::cout << "  curl --http2 https://localhost:8443/static/yourfile.txt --insecure\n\n";
    std::cout << "  # HTTP/1.1 降级测试\n";
    std::cout << "  curl --http1.1 https://localhost:8443/static/yourfile.txt --insecure\n\n";
    std::cout << "特性：\n";
    std::cout << "  ✓ 统一的 mount() 接口（HTTP/2 和 HTTP/1.1）\n";
    std::cout << "  ✓ 自动协议降级\n";
    std::cout << "  ✓ HTTP/2 自动分片（16KB）\n";
    std::cout << "  ✓ HTTP/1.1 零拷贝传输（sendfile）\n";
    std::cout << "  ✓ 实时进度监控（两种协议）\n";
    std::cout << "  ✓ 断点续传支持（HTTP/1.1 Range）\n";
    std::cout << "  ✓ 安全的路径检查\n";
    std::cout << "========================================\n";
    
    // ========================================
    // 核心调用：使用两个路由器启动服务器
    // ========================================
    server.run(runtime, http2Router, http1Router);
    server.wait();
    
    std::cout << "服务器已停止\n";
    return 0;
}

