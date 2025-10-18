// ========== 性能调试开关 ==========
// 取消注释下面这行可以启用详细的 sendfile 性能日志
// 注意：启用后会严重影响性能！仅用于诊断问题
// #define ENABLE_SENDFILE_PERFORMANCE_DEBUG
// ==================================

#include "galay/kernel/runtime/Runtime.h"
#include "kernel/http/HttpRouter.h"
#include "server/HttpServer.h"
#include "utils/HttpLogger.h"
#include <iostream>
#include <signal.h>
#include <cstddef>  // for SIZE_MAX

using namespace galay;
using namespace galay::http;

// 初始化信号处理，防止 SIGPIPE 导致程序崩溃
// SIGPIPE 会在向已关闭的连接发送数据时触发（包括 send() 和 sendfile()）
void initSignalHandling() {
    // 使用 sigaction 忽略 SIGPIPE 信号
    struct sigaction sa;
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPIPE, &sa, nullptr) == -1) {
        std::cerr << "Warning: Failed to set SIGPIPE handler" << std::endl;
    }
}

int main()
{
    // 必须在程序最开始就设置信号处理
    // 因为 sendfile() 不支持 MSG_NOSIGNAL，只能通过全局信号处理避免 SIGPIPE
    initSignalHandling();
    
    HttpLogger::getInstance()->getLogger()->getSpdlogger()->set_level(spdlog::level::level_enum::debug);
    
    RuntimeBuilder runtimeBuilder;
    auto runtime = runtimeBuilder.build();
    runtime.start();
    
    HttpServerBuilder builder;
    HttpServer server = builder.build();
    server.listen(Host("0.0.0.0", 80));
    
    // 创建路由器并挂载静态文件目录
    HttpRouter router;
    
    // 挂载静态文件服务
    // 注意：mount() 会立即验证路径，如果路径不存在会抛出异常
    try {
        // 方式1: 使用 chunked 传输（默认，内存占用小，但浏览器无法显示完整进度）
        // router.mount("/static", "/home/ubuntu/static", {
        //     .chunk_buffer_size = 128*1024,     // 128KB 缓冲区
        //     .use_chunked_transfer = true        // 默认值
        // });
        
        // 方式2: 使用 Content-Length 传输（浏览器显示完整进度，但需要一次性读取文件到内存）
        // router.mount("/static", "/home/ubuntu/static", {
        //     .use_chunked_transfer = false       // 禁用 chunked，使用 Content-Length
        // });
        
        // 方式3: 使用 sendfile 零拷贝传输（仅 Linux，性能最佳，浏览器显示完整进度，支持断点续传）
        router.mount("/static", "/home/ubuntu/static", {
            .use_sendfile = true,                // 启用 sendfile（底层自动循环发送）
            .sendfile_chunk_size = SIZE_MAX,     // 不分块，让底层 sendfile 循环自动处理
            .support_range = true                 // 支持 HTTP Range 断点续传（默认开启）
        });
        
        // 也可以挂载多个目录，使用不同的传输模式
        // router.mount("/assets", "./assets", {.use_chunked_transfer = true});      // 小文件用 chunked
        // router.mount("/videos", "./videos", {.use_sendfile = true});              // 大文件用 sendfile（零拷贝）
        // router.mount("/images", "./images", {.use_chunked_transfer = false});     // 需要进度的用 content-length
        
    } catch (const std::runtime_error& e) {
        std::cerr << "Mount failed: " << e.what() << std::endl;
        std::cerr << "Please ensure the directory exists before starting the server." << std::endl;
        return 1;
    }
    
    std::cout << "Static file server started on http://0.0.0.0:80" << std::endl;
    std::cout << "Try: http://localhost:80/static/index.html" << std::endl;
    
    server.run(runtime, router);
    server.wait();
    server.stop();
    
    return 0;
}

