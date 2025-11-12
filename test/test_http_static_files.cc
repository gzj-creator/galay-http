// ========== 调试开关 ==========
// 取消注释下面这行可以启用所有 debug 日志
// 注意：启用后会严重影响性能！仅用于诊断问题
// #define ENABLE_DEBUG
// ==================================

#include "galay/kernel/runtime/Runtime.h"
#include "kernel/http/HttpRouter.h"
#include "server/HttpServer.h"
#include "utils/HttpLogger.h"
#include <iostream>
#include <sstream>
#include <signal.h>
#include <cstddef>  // for SIZE_MAX
#include <chrono>
#include <iomanip>
#include <unordered_map>
#include <mutex>

using namespace galay;
using namespace galay::http;

// 文件传输状态跟踪
struct TransferState {
    std::chrono::steady_clock::time_point start_time;
    size_t last_bytes = 0;
    std::chrono::steady_clock::time_point last_update;
    size_t total_size = 0;
    std::string file_name;
    
    double getProgress() const {
        return total_size > 0 ? (last_bytes * 100.0) / total_size : 0.0;
    }
    
    double getSpeed() const {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - start_time).count() / 1000.0;
        return elapsed > 0 ? (last_bytes / (1024.0 * 1024.0)) / elapsed : 0.0;
    }
};

// 全局传输状态映射（使用客户端地址作为键）
std::unordered_map<std::string, TransferState> g_transfers;
std::mutex g_transfer_mutex;

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

// 格式化文件大小
std::string formatSize(size_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    int unit_idx = 0;
    double size = static_cast<double>(bytes);
    
    while (size >= 1024.0 && unit_idx < 3) {
        size /= 1024.0;
        unit_idx++;
    }
    
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << size << " " << units[unit_idx];
    return oss.str();
}

// 文件传输进度回调
void onFileTransferProgress(const HttpRequest& request, 
                            size_t bytes_sent, 
                            size_t total_bytes,
                            const FileTransferInfo& file_info) {
    // 使用文件路径作为唯一标识
    // 注意：如果多个客户端同时下载同一文件，进度会合并显示（但不影响实际传输）
    std::string transfer_id = file_info.relative_path;
    
    std::lock_guard<std::mutex> lock(g_transfer_mutex);
    
    auto& state = g_transfers[transfer_id];
    auto now = std::chrono::steady_clock::now();
    
    // 首次传输，初始化状态
    if (state.last_bytes == 0) {
        state.start_time = now;
        state.last_update = now;
        state.total_size = total_bytes;
        state.file_name = file_info.relative_path;
        
        std::cout << "\n📁 [开始传输] " << file_info.relative_path 
                  << " (" << formatSize(total_bytes) << ")"
                  << (file_info.is_range_request ? " [断点续传]" : "")
                  << std::endl;
        
        if (file_info.is_range_request) {
            std::cout << "   Range: " << file_info.range_start 
                      << "-" << file_info.range_end 
                      << " / " << file_info.file_size << std::endl;
        }
    }
    
    state.last_bytes = bytes_sent;
    state.last_update = now;
    
    // 计算进度和速度
    double progress = state.getProgress();
    double speed = state.getSpeed();
    
    // 每 10% 打印一次进度，或者传输完成时
    static std::unordered_map<std::string, int> last_progress_map;
    int current_progress_step = static_cast<int>(progress / 10);
    
    if (bytes_sent == total_bytes || 
        last_progress_map[transfer_id] != current_progress_step) {
        
        last_progress_map[transfer_id] = current_progress_step;
        
        std::cout << "📊 [" << std::fixed << std::setprecision(1) << progress << "%] "
                  << file_info.relative_path 
                  << " - " << formatSize(bytes_sent) << " / " << formatSize(total_bytes)
                  << " @ " << std::fixed << std::setprecision(2) << speed << " MB/s";
        
        // 传输完成
        if (bytes_sent == total_bytes) {
            auto total_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - state.start_time).count() / 1000.0;
            std::cout << " ✓ [完成，耗时 " << std::fixed << std::setprecision(2) 
                      << total_time << "s]";
            
            // 传输完成，清理状态
            g_transfers.erase(transfer_id);
            last_progress_map.erase(transfer_id);
        }
        
        std::cout << std::endl;
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
    
    // 挂载静态文件服务（带进度监控）
    // 注意：mount() 会立即验证路径，如果路径不存在会抛出异常
    try {
        // 方式1: 使用 chunked 传输（默认，内存占用小，但浏览器无法显示完整进度）
        // 适合小文件，内存占用小，但进度回调会被频繁调用
        // router.mount("/static", "/home/ubuntu/static", onFileTransferProgress, {
        //     .chunk_buffer_size = 128*1024,     // 128KB 缓冲区
        //     .use_chunked_transfer = true        // 默认值
        // });
        
        // 方式2: 使用 Content-Length 传输（浏览器显示完整进度，但需要一次性读取文件到内存）
        // 适合中等大小文件（几MB到几十MB），进度显示精确，但内存占用较大
        // router.mount("/static", "/home/ubuntu/static", onFileTransferProgress, {
        //     .use_chunked_transfer = false       // 禁用 chunked，使用 Content-Length
        // });
        
        // 方式3: 使用 sendfile 零拷贝传输（仅 Linux，性能最佳，浏览器显示完整进度，支持断点续传）
        // 适合大文件，性能最佳，支持断点续传，进度回调调用次数少
        router.mount("/static", "/home/ubuntu/static", onFileTransferProgress, {
            .use_sendfile = true,                // 启用 sendfile（底层自动循环发送）
            .sendfile_chunk_size = SIZE_MAX,     // 不分块，让底层 sendfile 循环自动处理
            .support_range = true                 // 支持 HTTP Range 断点续传（默认开启）
        });
        
        // 也可以挂载多个目录，使用不同的传输模式和进度回调
        // router.mount("/assets", "./assets", onFileTransferProgress, {.use_chunked_transfer = true});      // 小文件用 chunked
        // router.mount("/videos", "./videos", onFileTransferProgress, {.use_sendfile = true});              // 大文件用 sendfile（零拷贝）
        // router.mount("/images", "./images", onFileTransferProgress, {.use_chunked_transfer = false});     // 需要进度的用 content-length
        
    } catch (const std::runtime_error& e) {
        std::cerr << "❌ Mount failed: " << e.what() << std::endl;
        std::cerr << "Please ensure the directory exists before starting the server." << std::endl;
        return 1;
    }
    
    std::cout << "\n==============================================\n";
    std::cout << "🚀 静态文件服务器已启动（带进度监控）\n";
    std::cout << "==============================================\n";
    std::cout << "📍 监听地址: http://0.0.0.0:80\n";
    std::cout << "📁 静态目录: /home/ubuntu/static -> /static\n";
    std::cout << "⚡ 传输模式: sendfile 零拷贝 + 断点续传\n";
    std::cout << "📊 进度监控: 已启用\n";
    std::cout << "==============================================\n";
    std::cout << "\n示例访问：\n";
    std::cout << "  curl http://localhost:80/static/index.html\n";
    std::cout << "  curl http://localhost:80/static/large_file.zip -o file.zip\n";
    std::cout << "  curl -H \"Range: bytes=1024-2047\" http://localhost:80/static/video.mp4\n";
    std::cout << "\n等待请求中...\n\n" << std::endl;
    
    server.run(runtime, router);
    server.wait();
    server.stop();
    
    return 0;
}

