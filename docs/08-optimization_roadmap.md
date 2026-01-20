# HttpRouter 静态文件服务优化建议清单

## 🎯 性能优化

### 1. 文件缓存机制 ⭐⭐⭐⭐⭐
**优先级**: 高

**当前问题**:
- 每次请求都需要打开文件、读取内容
- 小文件频繁访问时 I/O 开销大
- `StaticFileConfig` 有 `m_enable_cache` 字段但未实现

**优化方案**:
```cpp
// 添加文件缓存
class FileCache {
    struct CacheEntry {
        std::string content;
        std::string mimeType;
        std::chrono::steady_clock::time_point lastAccess;
        size_t accessCount;
    };

    std::unordered_map<std::string, CacheEntry> m_cache;
    size_t m_maxCacheSize;
    size_t m_currentSize;

    // LRU 淘汰策略
    // 支持热点文件预加载
};
```

**预期收益**:
- 小文件访问性能提升 80%+
- 减少磁盘 I/O
- 降低 CPU 占用

---

### 2. 文件元数据缓存 ⭐⭐⭐⭐
**优先级**: 高

**当前问题**:
- 每次请求都调用 `fs::file_size()` 和 `fs::canonical()`
- 路径安全检查重复执行
- MIME 类型重复计算

**优化方案**:
```cpp
struct FileMetadata {
    size_t size;
    std::string canonicalPath;
    std::string mimeType;
    std::chrono::system_clock::time_point lastModified;
    bool isValid;
};

// 元数据缓存，定期刷新
std::unordered_map<std::string, FileMetadata> m_metadataCache;
```

**预期收益**:
- 减少系统调用
- 路径验证性能提升 50%+

---

### 3. 零拷贝优化 - mmap ⭐⭐⭐⭐
**优先级**: 中高

**当前问题**:
- MEMORY 模式仍需要将文件读入用户空间
- 中等文件（100KB-1MB）可以使用 mmap

**优化方案**:
```cpp
// 添加 MMAP 模式
case FileTransferMode::MMAP: {
    int fd = open(filePath.c_str(), O_RDONLY);
    void* mapped = mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fd, 0);

    // 直接从映射内存发送
    co_await writer.send(static_cast<char*>(mapped), fileSize);

    munmap(mapped, fileSize);
    close(fd);
}
```

**预期收益**:
- 中等文件性能提升 30-40%
- 减少内存拷贝

---

### 4. 异步文件 I/O (io_uring) ⭐⭐⭐⭐⭐
**优先级**: 高（Linux）

**当前问题**:
- CHUNK 模式使用同步 `read()` 调用
- 阻塞协程执行

**优化方案**:
```cpp
#ifdef USE_IOURING
// 使用 io_uring 异步读取文件
while (remaining > 0) {
    auto result = co_await asyncRead(file_fd, buffer, chunkSize);
    co_await writer.sendChunk(result.data, false);
}
#endif
```

**预期收益**:
- 高并发场景性能提升 2-3倍
- 更好的 CPU 利用率

---

### 5. HTTP Range 支持 ⭐⭐⭐⭐
**优先级**: 中高

**当前问题**:
- 不支持断点续传
- 不支持视频拖拽播放
- 大文件下载中断需要重新开始

**优化方案**:
```cpp
// 解析 Range 请求头
if (req.header().headerPairs().hasKey("Range")) {
    auto range = parseRange(req.header().headerPairs().getValue("Range"));

    // 206 Partial Content
    response.header().code() = HttpStatusCode::PartialContent_206;
    response.header().headerPairs().addHeaderPair(
        "Content-Range",
        fmt::format("bytes {}-{}/{}", range.start, range.end, fileSize)
    );

    // 使用 sendfile 的 offset 参数
    co_await conn.socket().sendfile(file_fd, range.start, range.length);
}
```

**预期收益**:
- 支持断点续传
- 视频播放体验提升
- 节省带宽

---

### 6. ETag 和条件请求支持 ⭐⭐⭐⭐
**优先级**: 中高

**当前问题**:
- 不支持 `If-None-Match` / `If-Modified-Since`
- 客户端无法利用缓存
- 浪费带宽

**优化方案**:
```cpp
// 生成 ETag (文件路径 + 修改时间 + 大小)
std::string etag = generateETag(filePath, lastModified, fileSize);
response.header().headerPairs().addHeaderPair("ETag", etag);
response.header().headerPairs().addHeaderPair("Last-Modified", formatHttpDate(lastModified));

// 检查条件请求
if (req.header().headerPairs().hasKey("If-None-Match")) {
    if (req.header().headerPairs().getValue("If-None-Match") == etag) {
        response.header().code() = HttpStatusCode::NotModified_304;
        co_await writer.send(response.toHeaderString());
        co_return;
    }
}
```

**预期收益**:
- 减少 60-80% 的重复传输
- 降低服务器负载
- 提升用户体验

---

### 7. 压缩支持 (gzip/brotli) ⭐⭐⭐⭐
**优先级**: 中高

**当前问题**:
- 不支持内容压缩
- 文本文件传输效率低

**优化方案**:
```cpp
// 检查 Accept-Encoding
if (req.header().headerPairs().hasKey("Accept-Encoding")) {
    auto encoding = req.header().headerPairs().getValue("Accept-Encoding");

    if (encoding.find("br") != std::string::npos && shouldCompress(mimeType)) {
        // Brotli 压缩
        auto compressed = brotliCompress(content);
        response.header().headerPairs().addHeaderPair("Content-Encoding", "br");
        response.setBodyStr(std::move(compressed));
    } else if (encoding.find("gzip") != std::string::npos) {
        // Gzip 压缩
        auto compressed = gzipCompress(content);
        response.header().headerPairs().addHeaderPair("Content-Encoding", "gzip");
        response.setBodyStr(std::move(compressed));
    }
}
```

**预期收益**:
- 文本文件传输大小减少 70-80%
- 带宽节省显著
- 加载速度提升

---

### 8. 预压缩文件支持 ⭐⭐⭐
**优先级**: 中

**当前问题**:
- 每次请求都需要压缩
- CPU 开销大

**优化方案**:
```cpp
// 查找预压缩文件
if (acceptsBrotli) {
    std::string brPath = filePath + ".br";
    if (fs::exists(brPath)) {
        // 直接发送预压缩文件
        response.header().headerPairs().addHeaderPair("Content-Encoding", "br");
        sendFileContent(conn, brPath, fs::file_size(brPath), mimeType, config);
        co_return;
    }
}
```

**预期收益**:
- 消除运行时压缩开销
- 性能提升 50%+

---

## 🔒 安全性优化

### 9. 路径遍历防护增强 ⭐⭐⭐⭐⭐
**优先级**: 高

**当前问题**:
- 只检查 canonical path
- 可能存在符号链接绕过
- 没有黑名单机制

**优化方案**:
```cpp
// 增强安全检查
bool isPathSafe(const fs::path& path, const fs::path& baseDir) {
    // 1. 检查符号链接
    if (fs::is_symlink(path)) {
        auto target = fs::read_symlink(path);
        if (!isUnderDirectory(target, baseDir)) {
            return false;
        }
    }

    // 2. 黑名单检查
    static const std::vector<std::string> blacklist = {
        ".git", ".env", ".htaccess", "web.config"
    };

    // 3. 检查隐藏文件
    if (path.filename().string()[0] == '.') {
        return false;
    }

    return true;
}
```

**预期收益**:
- 提升安全性
- 防止敏感文件泄露

---

### 10. 访问控制和权限管理 ⭐⭐⭐⭐
**优先级**: 中高

**当前问题**:
- 没有访问控制机制
- 所有文件都可以访问

**优化方案**:
```cpp
class AccessControl {
public:
    // IP 白名单/黑名单
    bool checkIP(const std::string& ip);

    // 基于路径的权限控制
    bool checkPath(const std::string& path, const std::string& user);

    // 速率限制
    bool checkRateLimit(const std::string& ip);
};
```

**预期收益**:
- 增强安全性
- 防止滥用

---

## 🛠️ 功能增强

### 11. 目录索引 (Directory Listing) ⭐⭐⭐
**优先级**: 中

**当前问题**:
- 访问目录返回 404
- 不支持目录浏览

**优化方案**:
```cpp
if (fs::is_directory(canonicalFile)) {
    if (config.isDirectoryListingEnabled()) {
        // 生成目录索引 HTML
        std::string html = generateDirectoryListing(canonicalFile, requestPath);
        response.setBodyStr(std::move(html));
    } else {
        // 查找 index.html
        auto indexPath = canonicalFile / "index.html";
        if (fs::exists(indexPath)) {
            sendFileContent(conn, indexPath.string(), ...);
        }
    }
}
```

**预期收益**:
- 更好的用户体验
- 类似 Nginx autoindex

---

### 12. 虚拟主机支持 ⭐⭐⭐
**优先级**: 中

**当前问题**:
- 不支持基于 Host 的路由
- 无法实现多站点

**优化方案**:
```cpp
// 基于 Host 头的路由
std::string host = req.header().headerPairs().getValue("Host");
auto siteConfig = m_virtualHosts[host];

if (siteConfig) {
    router.mount("/", siteConfig.documentRoot, siteConfig.fileConfig);
}
```

**预期收益**:
- 支持多站点部署
- 更灵活的配置

---

### 13. 文件监控和热更新 ⭐⭐⭐
**优先级**: 中

**当前问题**:
- `mountHardly()` 文件更新需要重启
- 缓存文件更新不及时

**优化方案**:
```cpp
#ifdef __linux__
// 使用 inotify 监控文件变化
class FileWatcher {
    void watch(const std::string& path) {
        int fd = inotify_init();
        inotify_add_watch(fd, path.c_str(), IN_MODIFY | IN_CREATE | IN_DELETE);
    }

    void onFileChanged(const std::string& path) {
        // 清除缓存
        m_cache.invalidate(path);
        // 重新注册路由
        reloadFile(path);
    }
};
#endif
```

**预期收益**:
- 支持热更新
- 开发体验提升

---

### 14. 多文件并发传输优化 ⭐⭐⭐
**优先级**: 中

**当前问题**:
- 每个文件独立处理
- HTTP/2 Server Push 未利用

**优化方案**:
```cpp
// HTTP/2 Server Push
if (isHttp2 && shouldPush(filePath)) {
    // 推送关联资源
    for (auto& resource : getLinkedResources(filePath)) {
        conn.push(resource);
    }
}
```

**预期收益**:
- 页面加载速度提升
- 减少往返延迟

---

## 📊 监控和诊断

### 15. 性能指标收集 ⭐⭐⭐⭐
**优先级**: 中高

**当前问题**:
- 没有性能监控
- 无法分析瓶颈

**优化方案**:
```cpp
struct FileMetrics {
    std::atomic<uint64_t> totalRequests{0};
    std::atomic<uint64_t> cacheHits{0};
    std::atomic<uint64_t> cacheMisses{0};
    std::atomic<uint64_t> bytesTransferred{0};
    std::atomic<uint64_t> totalLatency{0};

    // 按文件统计
    std::unordered_map<std::string, FileStats> perFileStats;
};
```

**预期收益**:
- 性能可观测
- 便于优化决策

---

### 16. 详细的错误日志 ⭐⭐⭐
**优先级**: 中

**当前问题**:
- 错误日志不够详细
- 难以排查问题

**优化方案**:
```cpp
HTTP_LOG_ERROR("File transfer failed: path={}, size={}, mode={}, error={}",
               filePath, fileSize, modeToString(mode), error.message());

// 添加请求追踪 ID
HTTP_LOG_INFO("[{}] Request: {} {} from {}",
              requestId, method, uri, clientIP);
```

**预期收益**:
- 更好的可调试性
- 快速定位问题

---

## 🔧 代码质量

### 17. 错误处理改进 ⭐⭐⭐⭐
**优先级**: 高

**当前问题**:
- 文件打开失败返回 500
- 没有区分不同错误类型
- 缺少重试机制

**优化方案**:
```cpp
// 更细粒度的错误处理
if (errno == EACCES) {
    response.header().code() = HttpStatusCode::Forbidden_403;
} else if (errno == ENOENT) {
    response.header().code() = HttpStatusCode::NotFound_404;
} else if (errno == EMFILE || errno == ENFILE) {
    response.header().code() = HttpStatusCode::ServiceUnavailable_503;
    response.header().headerPairs().addHeaderPair("Retry-After", "60");
}
```

**预期收益**:
- 更准确的错误响应
- 更好的用户体验

---

### 18. 资源泄漏防护 ⭐⭐⭐⭐⭐
**优先级**: 高

**当前问题**:
- 文件描述符可能泄漏
- 异常情况下 `close()` 未调用

**优化方案**:
```cpp
// 使用 RAII 管理文件描述符
class FileDescriptor {
    int m_fd;
public:
    FileDescriptor(const char* path, int flags)
        : m_fd(open(path, flags)) {}

    ~FileDescriptor() {
        if (m_fd >= 0) close(m_fd);
    }

    int get() const { return m_fd; }
    bool valid() const { return m_fd >= 0; }
};

// 使用
FileDescriptor fd(filePath.c_str(), O_RDONLY);
if (!fd.valid()) { /* error */ }
co_await conn.socket().sendfile(fd.get(), offset, size);
// 自动关闭
```

**预期收益**:
- 防止资源泄漏
- 提升稳定性

---

### 19. 配置验证 ⭐⭐⭐
**优先级**: 中

**当前问题**:
- 配置参数没有验证
- 可能设置不合理的值

**优化方案**:
```cpp
void StaticFileConfig::setChunkSize(size_t size) {
    if (size < 4096 || size > 10 * 1024 * 1024) {
        throw std::invalid_argument("Chunk size must be between 4KB and 10MB");
    }
    m_chunk_size = size;
}

bool StaticFileConfig::validate() const {
    if (m_small_file_threshold >= m_large_file_threshold) {
        return false;
    }
    return true;
}
```

**预期收益**:
- 防止配置错误
- 更好的错误提示

---

### 20. 单元测试覆盖率提升 ⭐⭐⭐
**优先级**: 中

**当前问题**:
- 缺少边界条件测试
- 缺少错误场景测试

**优化方案**:
```cpp
// 添加更多测试用例
- 空文件测试
- 超大文件测试 (>4GB)
- 并发访问测试
- 文件权限测试
- 磁盘满测试
- 网络中断测试
```

**预期收益**:
- 提升代码质量
- 减少 bug

---

## 📈 优先级总结

### 🔥 立即实施（高优先级）
1. **文件缓存机制** - 性能提升最大
2. **文件元数据缓存** - 减少系统调用
3. **异步文件 I/O** - 高并发性能
4. **HTTP Range 支持** - 用户体验
5. **ETag 支持** - 带宽节省
6. **路径遍历防护增强** - 安全性
7. **资源泄漏防护** - 稳定性

### ⚡ 近期实施（中高优先级）
8. 零拷贝优化 (mmap)
9. 压缩支持
10. 访问控制
11. 性能指标收集
12. 错误处理改进

### 💡 长期规划（中优先级）
13. 预压缩文件支持
14. 目录索引
15. 虚拟主机支持
16. 文件监控和热更新
17. 多文件并发传输
18. 详细错误日志
19. 配置验证
20. 单元测试覆盖率

---

## 🎯 实施建议

### 第一阶段（1-2周）
- 实现文件缓存机制（LRU）
- 实现元数据缓存
- 增强路径安全检查
- 修复资源泄漏问题

### 第二阶段（2-3周）
- 实现 HTTP Range 支持
- 实现 ETag 和条件请求
- 添加压缩支持
- 实现异步文件 I/O（Linux）

### 第三阶段（3-4周）
- 实现访问控制
- 添加性能监控
- 实现 mmap 优化
- 完善错误处理

### 第四阶段（长期）
- 其他功能增强
- 持续优化和测试

---

**总计**: 20个优化点
**预期整体性能提升**: 3-5倍（高并发场景）
**预期带宽节省**: 60-70%（启用缓存和压缩）
