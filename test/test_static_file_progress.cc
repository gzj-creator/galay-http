// ========== 调试开关 ==========
// 取消注释下面这行可以启用所有 debug 日志
// 注意：启用后会严重影响性能！仅用于诊断问题
// #define ENABLE_DEBUG
// ==================================

#include "galay/kernel/runtime/Runtime.h"
#include "kernel/http/HttpRouter.h"
#include "server/HttpServer.h"
#include "utils/HttpLogger.h"
#include "utils/HttpDebugLog.h"
#include <signal.h>
#include <cstddef>
#include <chrono>
#include <unordered_map>
#include <mutex>

using namespace galay;
using namespace galay::http;

// 传输状态跟踪（线程安全）
struct TransferState {
    std::chrono::steady_clock::time_point start_time;
    size_t last_bytes = 0;
    std::chrono::steady_clock::time_point last_update;
};

std::unordered_map<std::string, TransferState> g_transfers;
std::mutex g_transfer_mutex;

// 初始化信号处理，防止 SIGPIPE 导致程序崩溃
void initSignalHandling() {
    struct sigaction sa;
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPIPE, &sa, nullptr) == -1) {
        HTTP_LOG_WARN("Failed to set SIGPIPE handler");
    }
}

int main()
{
    initSignalHandling();
    
    HttpLogger::getInstance()->getLogger()->getSpdlogger()->set_level(spdlog::level::level_enum::info);
    
    RuntimeBuilder runtimeBuilder;
    auto runtime = runtimeBuilder.build();
    runtime.start();
    
    HttpServerBuilder builder;
    HttpServer server = builder.build();
    server.listen(Host("0.0.0.0", 8080));
    
    // 创建路由器并挂载静态文件目录
    HttpRouter router;
    
    try {
        // 定义进度回调函数
        auto progress_callback = [](const HttpRequest& request, 
                                     size_t bytes_sent, 
                                     size_t total_bytes,
                                     const FileTransferInfo& file_info) {
            std::lock_guard<std::mutex> lock(g_transfer_mutex);
            
            // 生成唯一的传输 ID（基于请求 URI + 客户端 IP）
            std::string transfer_id = file_info.relative_path;
            
            auto now = std::chrono::steady_clock::now();
            
            // 如果是新传输，初始化状态
            if (bytes_sent == 0) {
                TransferState state;
                state.start_time = now;
                state.last_bytes = 0;
                state.last_update = now;
                g_transfers[transfer_id] = state;
                
                HTTP_LOG_INFO("========================================");
                HTTP_LOG_INFO("📥 New Transfer Started");
                HTTP_LOG_INFO("File: {}", file_info.relative_path);
                HTTP_LOG_INFO("Path: {}", file_info.file_path);
                HTTP_LOG_INFO("MIME: {}", file_info.mime_type);
                HTTP_LOG_INFO("Size: {:.2f} MB", file_info.file_size / 1024.0 / 1024.0);
                if (file_info.is_range_request) {
                    HTTP_LOG_INFO("Range: {}-{} ({:.2f} MB)", 
                                  file_info.range_start, file_info.range_end,
                                  file_info.getTransferSize() / 1024.0 / 1024.0);
                }
                HTTP_LOG_INFO("========================================");
                return;
            }
            
            // 获取状态
            auto& state = g_transfers[transfer_id];
            
            // 计算进度百分比
            double progress = (bytes_sent * 100.0) / total_bytes;
            
            // 计算总体平均速度
            auto total_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.start_time).count();
            double avg_speed_mbps = 0.0;
            if (total_elapsed > 0) {
                avg_speed_mbps = (bytes_sent / (1024.0 * 1024.0)) / (total_elapsed / 1000.0);
            }
            
            // 计算瞬时速度（基于上次更新，使用微秒获得更高精度）
            auto interval_us = std::chrono::duration_cast<std::chrono::microseconds>(now - state.last_update).count();
            double instant_speed_mbps = avg_speed_mbps;  // 默认使用平均速度
            
            // 只有当时间间隔足够长（>= 10ms）时才计算瞬时速度，避免因间隔太短导致速度为0或不准确
            if (interval_us >= 10000) {  // 10毫秒 = 10000微秒
                size_t bytes_diff = bytes_sent - state.last_bytes;
                instant_speed_mbps = (bytes_diff / (1024.0 * 1024.0)) / (interval_us / 1000000.0);
            }
            
            // 估计剩余时间
            size_t remaining_bytes = total_bytes - bytes_sent;
            int eta_seconds = 0;
            if (avg_speed_mbps > 0) {
                eta_seconds = static_cast<int>(remaining_bytes / (1024.0 * 1024.0) / avg_speed_mbps);
            }
            
            // 输出进度信息
            HTTP_LOG_INFO("{} | {:.1f}% | {:.2f}/{:.2f} MB | Speed: {:.1f} MB/s | Avg: {:.1f} MB/s | ETA: {}s",
                          file_info.relative_path, progress,
                          bytes_sent / 1024.0 / 1024.0, total_bytes / 1024.0 / 1024.0,
                          instant_speed_mbps, avg_speed_mbps, eta_seconds);
            
            // 更新状态
            state.last_bytes = bytes_sent;
            state.last_update = now;
            
            // 如果传输完成
            if (bytes_sent >= total_bytes) {
                HTTP_LOG_INFO("✅ Transfer Complete: {}", file_info.relative_path);
                HTTP_LOG_INFO("   Total time: {:.2f} seconds", total_elapsed / 1000.0);
                HTTP_LOG_INFO("   Average speed: {:.1f} MB/s", avg_speed_mbps);
                HTTP_LOG_INFO("========================================");
                
                // 清理状态
                g_transfers.erase(transfer_id);
            }
        };
        
        router.mount("/static", "/Users/gongzhijie/Downloads", progress_callback);
        
        HTTP_LOG_INFO("========================================");
        HTTP_LOG_INFO("Static File Server with Progress Monitoring");
        HTTP_LOG_INFO("Listening on: http://0.0.0.0:8080");
        HTTP_LOG_INFO("Mount point: /static -> /Users/gongzhijie/Downloads");
        HTTP_LOG_INFO("Features:");
        HTTP_LOG_INFO("  ✓ Default settings (sendfile on Linux)");
        HTTP_LOG_INFO("  ✓ Range support (resume downloads)");
        HTTP_LOG_INFO("  ✓ Real-time progress monitoring");
        HTTP_LOG_INFO("  ✓ Speed calculation");
        HTTP_LOG_INFO("========================================");
        
    } catch (const std::runtime_error& e) {
        HTTP_LOG_ERROR("Mount failed: {}", e.what());
        return 1;
    }
    
    server.run(runtime, router);
    server.wait();
    server.stop();
    
    return 0;
}

