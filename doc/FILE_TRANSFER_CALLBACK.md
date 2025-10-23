# 文件传输进度回调功能

## 概述

`mount()` 函数现在支持设置可选的文件传输进度回调，允许您在文件传输过程中实时监控进度、速度和其他传输信息。

## 核心类型

### FileTransferInfo

文件传输详细信息结构体：

```cpp
struct FileTransferInfo {
    std::string file_path;       // 完整文件路径（绝对路径）
    std::string relative_path;   // 相对于挂载点的路径（用户请求的路径）
    std::string mime_type;       // MIME 类型（如 "text/html", "image/png"）
    size_t file_size;            // 文件总大小（字节）
    size_t range_start;          // Range 请求的起始位置
    size_t range_end;            // Range 请求的结束位置
    bool is_range_request;       // 是否是 HTTP Range 请求（断点续传）
    
    // 获取实际传输的字节数
    size_t getTransferSize() const {
        return range_end - range_start + 1;
    }
};
```

### FileTransferProgressCallback

回调函数类型定义：

```cpp
using FileTransferProgressCallback = std::function<void(
    const HttpRequest&,           // HTTP 请求对象
    size_t,                       // 已发送字节数
    size_t,                       // 总字节数（要发送的）
    const FileTransferInfo&       // 文件详细信息
)>;
```

## 基本使用

### 示例 1: 简单的进度输出

```cpp
#include "galay-http/kernel/http/HttpRouter.h"
#include "galay-http/server/HttpServer.h"
#include <iostream>

HttpRouter router;
HttpSettings settings;

// 设置进度回调
settings.on_transfer_progress = [](const HttpRequest& req, 
                                    size_t sent, 
                                    size_t total,
                                    const FileTransferInfo& info) {
    double progress = (sent * 100.0) / total;
    std::cout << info.relative_path << ": " 
              << progress << "% (" << sent << "/" << total << " bytes)\n";
};

settings.use_sendfile = true;
settings.support_range = true;

router.mount("/downloads", "./files", settings);
```

**输出示例**：
```
video.mp4: 0% (0/104857600 bytes)
video.mp4: 5.2% (5450000/104857600 bytes)
video.mp4: 10.5% (11000000/104857600 bytes)
...
video.mp4: 100% (104857600/104857600 bytes)
```

### 示例 2: 带速度计算的进度条

```cpp
#include <chrono>
#include <iomanip>

struct TransferState {
    std::chrono::steady_clock::time_point start_time;
    size_t last_bytes = 0;
    std::chrono::steady_clock::time_point last_update;
};

std::unordered_map<std::string, TransferState> transfers;

settings.on_transfer_progress = [&transfers](const HttpRequest& req, 
                                              size_t bytes_sent, 
                                              size_t total_bytes,
                                              const FileTransferInfo& info) {
    auto now = std::chrono::steady_clock::now();
    
    // 新传输
    if (bytes_sent == 0) {
        TransferState state;
        state.start_time = now;
        state.last_bytes = 0;
        state.last_update = now;
        transfers[info.relative_path] = state;
        
        std::cout << "📥 Starting: " << info.relative_path 
                  << " (" << (info.file_size / 1024.0 / 1024.0) << " MB)\n";
        return;
    }
    
    auto& state = transfers[info.relative_path];
    
    // 计算进度
    double progress = (bytes_sent * 100.0) / total_bytes;
    
    // 计算平均速度
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - state.start_time).count();
    double avg_speed_mbps = (bytes_sent / (1024.0 * 1024.0)) / (elapsed_ms / 1000.0);
    
    // 计算瞬时速度
    auto interval_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - state.last_update).count();
    double instant_speed_mbps = 0.0;
    if (interval_ms > 0) {
        size_t bytes_diff = bytes_sent - state.last_bytes;
        instant_speed_mbps = (bytes_diff / (1024.0 * 1024.0)) / (interval_ms / 1000.0);
    }
    
    // 输出进度条
    std::cout << "\r" << info.relative_path << " | "
              << std::fixed << std::setprecision(1) << progress << "% | "
              << instant_speed_mbps << " MB/s" << std::flush;
    
    // 更新状态
    state.last_bytes = bytes_sent;
    state.last_update = now;
    
    // 传输完成
    if (bytes_sent >= total_bytes) {
        std::cout << "\n✅ Complete: " << info.relative_path 
                  << " (avg: " << avg_speed_mbps << " MB/s)\n";
        transfers.erase(info.relative_path);
    }
};
```

**输出示例**：
```
📥 Starting: video.mp4 (100.0 MB)
video.mp4 | 12.5% | 2.3 MB/s
video.mp4 | 25.0% | 2.5 MB/s
video.mp4 | 50.0% | 2.4 MB/s
video.mp4 | 75.0% | 2.6 MB/s
video.mp4 | 100.0% | 2.5 MB/s
✅ Complete: video.mp4 (avg: 2.4 MB/s)
```

### 示例 3: 流量统计和日志记录

```cpp
#include <fstream>
#include <mutex>

struct Statistics {
    size_t total_files = 0;
    size_t total_bytes = 0;
    std::mutex mutex;
};

Statistics stats;

settings.on_transfer_progress = [&stats](const HttpRequest& req, 
                                          size_t bytes_sent, 
                                          size_t total_bytes,
                                          const FileTransferInfo& info) {
    std::lock_guard<std::mutex> lock(stats.mutex);
    
    // 记录到日志文件
    if (bytes_sent == 0) {
        std::ofstream log("transfer.log", std::ios::app);
        log << "[" << std::time(nullptr) << "] START: " 
            << info.relative_path << " (" << total_bytes << " bytes)\n";
    } else if (bytes_sent >= total_bytes) {
        stats.total_files++;
        stats.total_bytes += bytes_sent;
        
        std::ofstream log("transfer.log", std::ios::app);
        log << "[" << std::time(nullptr) << "] COMPLETE: " 
            << info.relative_path << " (" << bytes_sent << " bytes)\n";
        log << "Total: " << stats.total_files << " files, "
            << (stats.total_bytes / 1024.0 / 1024.0) << " MB\n";
    }
};
```

## 高级用法

### 示例 4: 支持 Range 请求（断点续传）

```cpp
settings.on_transfer_progress = [](const HttpRequest& req, 
                                    size_t bytes_sent, 
                                    size_t total_bytes,
                                    const FileTransferInfo& info) {
    if (bytes_sent == 0) {
        std::cout << "File: " << info.relative_path << "\n";
        std::cout << "Total size: " << info.file_size << " bytes\n";
        
        if (info.is_range_request) {
            std::cout << "⚡ Range request (resume download)\n";
            std::cout << "Range: " << info.range_start << "-" << info.range_end << "\n";
            std::cout << "Sending: " << info.getTransferSize() << " bytes\n";
        } else {
            std::cout << "Sending entire file\n";
        }
    }
    
    // 显示进度
    double progress = (bytes_sent * 100.0) / total_bytes;
    std::cout << "\r" << progress << "%" << std::flush;
    
    if (bytes_sent >= total_bytes) {
        std::cout << "\n✅ Done!\n";
    }
};
```

### 示例 5: 并发传输监控

```cpp
#include <map>
#include <mutex>
#include <thread>

struct ActiveTransfer {
    std::string file_name;
    size_t total_bytes;
    size_t sent_bytes;
    std::chrono::steady_clock::time_point start_time;
};

std::map<std::thread::id, ActiveTransfer> active_transfers;
std::mutex transfer_mutex;

settings.on_transfer_progress = [](const HttpRequest& req, 
                                    size_t bytes_sent, 
                                    size_t total_bytes,
                                    const FileTransferInfo& info) {
    std::lock_guard<std::mutex> lock(transfer_mutex);
    auto thread_id = std::this_thread::get_id();
    
    if (bytes_sent == 0) {
        ActiveTransfer transfer;
        transfer.file_name = info.relative_path;
        transfer.total_bytes = total_bytes;
        transfer.sent_bytes = 0;
        transfer.start_time = std::chrono::steady_clock::now();
        active_transfers[thread_id] = transfer;
        
        std::cout << "Active transfers: " << active_transfers.size() << "\n";
    } else {
        active_transfers[thread_id].sent_bytes = bytes_sent;
    }
    
    if (bytes_sent >= total_bytes) {
        active_transfers.erase(thread_id);
        std::cout << "Completed. Remaining: " << active_transfers.size() << "\n";
    }
    
    // 显示所有活跃传输
    for (const auto& [tid, transfer] : active_transfers) {
        double progress = (transfer.sent_bytes * 100.0) / transfer.total_bytes;
        std::cout << "  " << transfer.file_name << ": " << progress << "%\n";
    }
};
```

## 回调调用时机

### Sendfile 模式（推荐）

```cpp
settings.use_sendfile = true;
```

- **开始时**：`bytes_sent = 0`
- **传输中**：每次底层 sendfile 返回后调用（通常每 300KB-500KB）
- **结束时**：`bytes_sent == total_bytes`

### Chunked 模式

```cpp
settings.use_chunked_transfer = true;
```

- **开始时**：`bytes_sent = 0`
- **传输中**：每发送一个 chunk 后调用（由 `chunk_buffer_size` 决定）
- **结束时**：`bytes_sent == total_bytes`

### Content-Length 模式

```cpp
settings.use_chunked_transfer = false;
settings.use_sendfile = false;
```

- **开始时**：`bytes_sent = 0`
- **结束时**：`bytes_sent == total_bytes`（文件一次性读取并发送）

## 性能考虑

### ⚠️ 回调性能影响

回调函数会在传输过程中被频繁调用，应避免：

```cpp
// ❌ 错误：阻塞操作
settings.on_transfer_progress = [](auto&, size_t sent, size_t total, auto& info) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // 阻塞！
    heavy_computation();  // 耗时操作！
};

// ✅ 正确：快速返回
settings.on_transfer_progress = [](auto&, size_t sent, size_t total, auto& info) {
    // 只做简单的计算和输出
    double progress = (sent * 100.0) / total;
    std::cout << "\r" << progress << "%" << std::flush;
};
```

### 建议

1. **保持回调简洁**：避免复杂计算和I/O操作
2. **使用异步日志**：如果需要记录日志，使用异步写入
3. **限制输出频率**：对于大文件，可以每隔一定百分比才输出一次
4. **线程安全**：如果多线程环境，使用互斥锁保护共享数据

## 完整示例

完整的进度监控服务器示例在 `test/test_static_file_progress.cc`，特性包括：

- ✅ 实时进度条
- ✅ 瞬时速度和平均速度
- ✅ 剩余时间估算（ETA）
- ✅ Range 请求支持
- ✅ 多文件并发传输监控
- ✅ 传输统计

运行示例：

```bash
cd build
./test/test_static_file_progress
```

然后在另一个终端下载文件：

```bash
wget http://localhost/static/large-file.bin
```

你会看到类似的输出：

```
========================================
📥 New Transfer Started
File: large-file.bin
Path: /home/ubuntu/static/large-file.bin
MIME: application/octet-stream
Size: 214.3 MB
========================================

large-file.bin | 12.5% | 26.8/214.3 MB | Speed: 2.5 MB/s | Avg: 2.3 MB/s | ETA: 75s
large-file.bin | 25.0% | 53.6/214.3 MB | Speed: 2.7 MB/s | Avg: 2.4 MB/s | ETA: 60s
...
large-file.bin | 100.0% | 214.3/214.3 MB | Speed: 2.6 MB/s | Avg: 2.5 MB/s | ETA: 0s

✅ Transfer Complete: large-file.bin
   Total time: 85.6 seconds
   Average speed: 2.5 MB/s
========================================
```

## API 参考

### HttpSettings::on_transfer_progress

**类型**：`FileTransferProgressCallback`

**默认值**：`nullptr`（不调用回调）

**说明**：文件传输进度回调函数，在文件传输过程中被调用

**参数**：
- `const HttpRequest& request` - HTTP 请求对象
- `size_t bytes_sent` - 已发送的字节数
- `size_t total_bytes` - 总共需要发送的字节数
- `const FileTransferInfo& file_info` - 文件详细信息

**示例**：
```cpp
HttpSettings settings;
settings.on_transfer_progress = [](const HttpRequest& req, 
                                    size_t sent, size_t total,
                                    const FileTransferInfo& info) {
    // 你的回调逻辑
};
router.mount("/path", "./dir", settings);
```

## 常见问题

### Q1: 回调会影响传输性能吗？

**A**: 会有一定影响，但如果回调函数足够简洁（只做简单计算和输出），影响很小（< 1%）。避免在回调中进行阻塞操作。

### Q2: 回调是在哪个线程调用的？

**A**: 回调在处理 HTTP 请求的协程线程中调用。如果你的回调访问共享数据，需要使用互斥锁。

### Q3: 可以在回调中取消传输吗？

**A**: 当前版本不支持。回调只用于监控，不能控制传输过程。

### Q4: 如何减少回调调用次数？

**A**: 可以在回调内部做频率控制：

```cpp
static size_t last_reported_percent = 0;

settings.on_transfer_progress = [](auto&, size_t sent, size_t total, auto& info) {
    size_t percent = (sent * 100) / total;
    if (percent > last_reported_percent) {
        last_reported_percent = percent;
        std::cout << percent << "%\n";
    }
};
```

### Q5: 如何获取客户端 IP？

**A**: 当前回调中不直接提供客户端 IP。如需此信息，可以从 `HttpRequest` 的 header 中获取（例如 X-Forwarded-For）。

## 总结

文件传输进度回调功能提供了：

- ✅ 实时传输进度监控
- ✅ 支持所有传输模式（sendfile、chunked、content-length）
- ✅ 支持 Range 请求（断点续传）
- ✅ 灵活的回调接口
- ✅ 低性能开销

非常适合用于：
- 文件下载进度显示
- 传输速度监控
- 流量统计
- 日志记录
- 性能分析

Happy coding! 🚀


