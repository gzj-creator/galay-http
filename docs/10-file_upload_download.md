# 文件上传下载完整指南

本文档详细介绍 Galay-HTTP 中文件上传和下载的实现方式，包括静态文件服务、Range 请求、ETag 缓存验证和断点续传机制。

## 目录

- [静态文件下载](#静态文件下载)
- [文件传输模式](#文件传输模式)
- [Range 请求](#range-请求)
- [ETag 缓存验证](#etag-缓存验证)
- [文件上传](#文件上传)
- [完整示例](#完整示例)

## 静态文件下载

### 基本用法

Galay-HTTP 通过 `HttpRouter::mount()` 方法提供静态文件服务：

```cpp
#include "galay-http/kernel/http/HttpServer.h"
#include "galay-http/kernel/http/HttpRouter.h"

using namespace galay::http;

int main() {
    HttpRouter router;

    // 挂载静态文件目录
    // GET /static/file.txt -> 读取 ./public/file.txt
    router.mount("/static", "./public");

    HttpServerConfig config;
    config.port = 8080;

    HttpServer server(config);
    server.start(std::move(router));

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}
```

### 安全特性

静态文件服务内置多项安全特性：

1. **启动时路径验证**: `mount()` 会在启动时验证目录是否存在
2. **路径遍历防护**: 自动阻止 `../` 攻击
3. **路径规范化**: 自动处理 `./`、`//` 等路径
4. **Content-Type 自动设置**: 根据文件扩展名自动设置 MIME 类型

**支持的文件类型**:
- HTML/CSS/JavaScript
- 图片（PNG, JPEG, GIF, SVG, WebP）
- JSON/XML
- PDF
- 视频（MP4, WebM）
- 音频（MP3, WAV, OGG）
- 其他（默认为 application/octet-stream）

## 文件传输模式

Galay-HTTP 支持四种文件传输模式，可根据文件大小自动选择最优方式。

### 传输模式说明

#### 1. MEMORY 模式（内存模式）

**适用场景**: 小文件（<64KB）

**特点**:
- 将文件完整读入内存后一次性发送
- 简单高效，延迟最低
- 内存占用较高

**实现原理**:
```cpp
// 伪代码
std::string fileContent = readFileToMemory(filePath);
response.setBody(fileContent);
co_await writer.sendResponse(response);
```

#### 2. CHUNK 模式（分块模式）

**适用场景**: 中等文件（64KB - 1MB）

**特点**:
- 使用 HTTP Chunked Transfer Encoding
- 支持流式传输，内存占用可控
- 客户端可以边接收边处理

**实现原理**:
```cpp
// 伪代码
co_await writer.sendHeader(header);  // Transfer-Encoding: chunked

while (!eof) {
    std::string chunk = readChunk(64KB);
    co_await writer.sendChunk(chunk, false);
}
co_await writer.sendChunk("", true);  // 最后一个 chunk
```

#### 3. SENDFILE 模式（零拷贝模式）

**适用场景**: 大文件（>1MB）

**特点**:
- 使用 `sendfile()` 系统调用
- 零拷贝，数据直接从文件系统传输到网络
- CPU 占用最低，性能最优
- 不支持 Chunked 编码

**实现原理**:
```cpp
// 伪代码
int fd = open(filePath);
off_t offset = 0;
size_t remaining = fileSize;

while (remaining > 0) {
    ssize_t sent = sendfile(socket_fd, fd, &offset, remaining);
    remaining -= sent;
}
```

#### 4. AUTO 模式（自动模式）

**推荐使用**: 根据文件大小自动选择最优模式

**决策逻辑**:
- 文件 ≤ 64KB → MEMORY
- 64KB < 文件 ≤ 1MB → CHUNK
- 文件 > 1MB → SENDFILE

### 配置传输模式

```cpp
#include "galay-http/kernel/http/StaticFileConfig.h"

StaticFileConfig config;

// 方式1: 使用 AUTO 模式（推荐）
config.setTransferMode(FileTransferMode::AUTO);

// 方式2: 手动指定模式
config.setTransferMode(FileTransferMode::SENDFILE);

// 自定义阈值
config.setSmallFileThreshold(128 * 1024);   // 128KB
config.setLargeFileThreshold(2 * 1024 * 1024);  // 2MB

// 设置 Chunk 大小
config.setChunkSize(64 * 1024);  // 64KB

// 设置 SendFile 块大小
config.setSendFileChunkSize(10 * 1024 * 1024);  // 10MB

// 挂载时使用配置
router.mount("/files", "./files", config);
```

### 性能对比

| 模式 | 文件大小 | 内存占用 | CPU 占用 | 延迟 | 吞吐量 |
|------|---------|---------|---------|------|--------|
| MEMORY | <64KB | 高 | 中 | 最低 | 高 |
| CHUNK | 64KB-1MB | 中 | 中 | 中 | 中 |
| SENDFILE | >1MB | 低 | 最低 | 低 | 最高 |

## Range 请求

Range 请求允许客户端请求文件的部分内容，是实现断点续传的基础。

### 服务端支持

静态文件挂载自动支持 Range 请求，无需额外代码。服务器会：

1. 解析 `Range` 请求头
2. 验证范围是否有效
3. 返回 `206 Partial Content` 状态码
4. 设置 `Content-Range` 响应头
5. 发送指定范围的数据

### 客户端发送 Range 请求

```cpp
#include "galay-http/kernel/http/HttpClient.h"

Coroutine downloadPartialFile(Runtime& runtime) {
    HttpClient client;
    co_await client.connect("http://example.com/large-file.zip");

    // 请求文件的前 1MB
    while (true) {
        auto result = co_await client.get("/large-file.zip", {
            {"Range", "bytes=0-1048575"}  // 0 到 1MB-1
        });

        if (!result || !result.value()) continue;

        auto response = result.value().value();

        // 检查状态码
        if (response.header().code() == HttpStatusCode::PARTIAL_CONTENT_206) {
            // 获取 Content-Range 头
            auto contentRange = response.header().headerPairs()
                .getHeaderPair("Content-Range");
            std::cout << "Content-Range: " << contentRange << "\n";
            // 输出: bytes 0-1048575/10485760

            // 保存数据
            std::string data = response.getBodyStr();
            std::ofstream file("output.zip", std::ios::binary);
            file.write(data.data(), data.size());
            file.close();
        }
        break;
    }

    co_await client.close();
    co_return;
}
```

### Range 请求格式

#### 1. 单范围请求

```cpp
// 请求前 500 字节
{"Range", "bytes=0-499"}

// 请求 500-999 字节
{"Range", "bytes=500-999"}
```

**响应**:
```
HTTP/1.1 206 Partial Content
Content-Range: bytes 0-499/10000
Content-Length: 500

[500 字节数据]
```

#### 2. 后缀范围请求

```cpp
// 从 1000 字节到文件末尾
{"Range", "bytes=1000-"}
```

**响应**:
```
HTTP/1.1 206 Partial Content
Content-Range: bytes 1000-9999/10000
Content-Length: 9000

[9000 字节数据]
```

#### 3. 前缀范围请求

```cpp
// 最后 500 字节
{"Range", "bytes=-500"}
```

**响应**:
```
HTTP/1.1 206 Partial Content
Content-Range: bytes 9500-9999/10000
Content-Length: 500

[500 字节数据]
```

#### 4. 多范围请求

```cpp
// 请求多个范围
{"Range", "bytes=0-99,200-299,500-599"}
```

**响应**:
```
HTTP/1.1 206 Partial Content
Content-Type: multipart/byteranges; boundary=multipart_boundary_xxx

--multipart_boundary_xxx
Content-Type: application/octet-stream
Content-Range: bytes 0-99/10000

[100 字节数据]
--multipart_boundary_xxx
Content-Type: application/octet-stream
Content-Range: bytes 200-299/10000

[100 字节数据]
--multipart_boundary_xxx
Content-Type: application/octet-stream
Content-Range: bytes 500-599/10000

[100 字节数据]
--multipart_boundary_xxx--
```

### Range 请求的错误处理

```cpp
auto result = co_await client.get("/file.zip", {
    {"Range", "bytes=0-999"}
});

if (!result || !result.value()) {
    std::cerr << "Request failed\n";
    co_return;
}

auto response = result.value().value();

switch (response.header().code()) {
    case HttpStatusCode::PARTIAL_CONTENT_206:
        // 成功，处理部分内容
        std::cout << "Received partial content\n";
        break;

    case HttpStatusCode::OK_200:
        // 服务器不支持 Range，返回完整文件
        std::cout << "Server doesn't support Range, got full file\n";
        break;

    case HttpStatusCode::RANGE_NOT_SATISFIABLE_416:
        // 范围无效
        std::cerr << "Invalid range\n";
        break;

    default:
        std::cerr << "Unexpected status: "
                  << static_cast<int>(response.header().code()) << "\n";
        break;
}
```

## ETag 缓存验证

ETag（Entity Tag）是文件的唯一标识符，用于验证文件是否被修改。

### ETag 生成

Galay-HTTP 支持两种 ETag：

#### 1. 强 ETag（默认）

基于文件的 inode、修改时间和大小生成：

```cpp
// 格式: "inode-size-mtime"
// 示例: "1a2b3c-1024-5f8d9e"
```

**特点**:
- 文件内容改变，ETag 必定改变
- 适用于需要严格验证的场景

#### 2. 弱 ETag

仅基于修改时间和大小生成：

```cpp
// 格式: W/"size-mtime"
// 示例: W/"1024-5f8d9e"
```

**特点**:
- 文件内容略有差异但语义相同时，ETag 可能相同
- 适用于可以容忍轻微差异的场景

### 客户端使用 ETag

#### 1. If-None-Match（条件 GET）

检查文件是否被修改，如果未修改则返回 304：

```cpp
Coroutine conditionalGet(HttpClient& client) {
    // 首次请求，获取 ETag
    auto result1 = co_await client.get("/file.txt");
    if (!result1 || !result1.value()) co_return;

    auto response1 = result1.value().value();
    std::string etag = response1.header().headerPairs()
        .getHeaderPair("ETag");
    std::cout << "ETag: " << etag << "\n";

    // 保存文件内容
    std::string content = response1.getBodyStr();

    // 后续请求，使用 If-None-Match
    while (true) {
        auto result2 = co_await client.get("/file.txt", {
            {"If-None-Match", etag}
        });

        if (!result2 || !result2.value()) continue;

        auto response2 = result2.value().value();

        if (response2.header().code() == HttpStatusCode::NOT_MODIFIED_304) {
            std::cout << "File not modified, use cached version\n";
            // 使用之前保存的 content
        } else if (response2.header().code() == HttpStatusCode::OK_200) {
            std::cout << "File modified, update cache\n";
            content = response2.getBodyStr();
            etag = response2.header().headerPairs().getHeaderPair("ETag");
        }
        break;
    }

    co_return;
}
```

#### 2. If-Match（条件 PUT）

确保只在文件未被修改时才更新：

```cpp
// 上传文件时使用 If-Match
auto result = co_await client.put("/file.txt", newContent, "text/plain", {
    {"If-Match", etag}
});

if (!result || !result.value()) co_return;

auto response = result.value().value();

if (response.header().code() == HttpStatusCode::PRECONDITION_FAILED_412) {
    std::cerr << "File was modified by another client\n";
} else if (response.header().code() == HttpStatusCode::OK_200) {
    std::cout << "File updated successfully\n";
}
```

#### 3. If-Range（条件 Range 请求）

确保文件未改变才使用 Range 请求，否则返回完整文件：

```cpp
auto result = co_await client.get("/file.zip", {
    {"Range", "bytes=1048576-"},  // 从 1MB 处继续
    {"If-Range", etag}            // 验证文件未改变
});

if (!result || !result.value()) co_return;

auto response = result.value().value();

if (response.header().code() == HttpStatusCode::PARTIAL_CONTENT_206) {
    std::cout << "File unchanged, resuming download\n";
    // 追加数据
} else if (response.header().code() == HttpStatusCode::OK_200) {
    std::cout << "File changed, restarting download\n";
    // 重新下载完整文件
}
```

## 文件上传

### 使用 POST 上传文件

```cpp
Coroutine uploadFile(HttpClient& client, const std::string& filePath) {
    // 读取文件内容
    std::ifstream file(filePath, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    file.close();

    // 发送 POST 请求
    while (true) {
        auto result = co_await client.post(
            "/upload",
            content,
            "application/octet-stream",
            {
                {"Content-Disposition", "attachment; filename=\"file.zip\""}
            }
        );

        if (!result || !result.value()) continue;

        auto response = result.value().value();

        if (response.header().code() == HttpStatusCode::OK_200 ||
            response.header().code() == HttpStatusCode::CREATED_201) {
            std::cout << "Upload successful\n";
        } else {
            std::cerr << "Upload failed: "
                      << static_cast<int>(response.header().code()) << "\n";
        }
        break;
    }

    co_return;
}
```

### 服务端处理文件上传

```cpp
Coroutine uploadHandler(HttpConn& conn, HttpRequest req) {
    // 获取上传的文件内容
    std::string fileContent = req.getBodyStr();

    // 获取文件名（从 Content-Disposition 头）
    std::string contentDisposition = req.header().headerPairs()
        .getHeaderPair("Content-Disposition");

    // 解析文件名（简化版）
    std::string filename = "uploaded_file";
    size_t pos = contentDisposition.find("filename=");
    if (pos != std::string::npos) {
        filename = contentDisposition.substr(pos + 10);  // 跳过 filename="
        filename = filename.substr(0, filename.find('"'));
    }

    // 保存文件
    std::string savePath = "./uploads/" + filename;
    std::ofstream outFile(savePath, std::ios::binary);
    outFile.write(fileContent.data(), fileContent.size());
    outFile.close();

    // 返回响应
    auto response = Http1_1ResponseBuilder::created()
        .header("Location", "/files/" + filename)
        .json(R"({"status": "success", "filename": ")" + filename + R"("})")
        .build();

    auto writer = conn.getWriter();
    co_await writer.sendResponse(response);
    co_await conn.close();
    co_return;
}
```

### 使用 Chunked 编码上传大文件

```cpp
Coroutine uploadLargeFile(HttpClient& client, const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);

    // 发送请求头
    HttpRequestHeader header;
    header.method() = HttpMethod::HttpMethod_Post;
    header.uri() = "/upload";
    header.headerPairs().addHeaderPair("Transfer-Encoding", "chunked");
    header.headerPairs().addHeaderPair("Content-Type", "application/octet-stream");

    auto writer = client.getWriter();
    co_await writer.sendHeader(std::move(header));

    // 分块发送文件
    const size_t chunkSize = 64 * 1024;  // 64KB
    char buffer[chunkSize];

    while (file.read(buffer, chunkSize) || file.gcount() > 0) {
        std::string chunk(buffer, file.gcount());
        co_await writer.sendChunk(chunk, false);
    }

    // 发送最后一个 chunk
    co_await writer.sendChunk("", true);

    file.close();

    // 接收响应
    auto reader = client.getReader();
    HttpResponse response;
    co_await reader.getResponse(response);

    std::cout << "Upload status: "
              << static_cast<int>(response.header().code()) << "\n";

    co_return;
}
```

## 完整示例

### 示例1: 静态文件服务器

```cpp
#include "galay-http/kernel/http/HttpServer.h"
#include "galay-http/kernel/http/HttpRouter.h"
#include "galay-http/kernel/http/StaticFileConfig.h"

using namespace galay::http;

int main() {
    HttpRouter router;

    // 配置静态文件传输
    StaticFileConfig config;
    config.setTransferMode(FileTransferMode::AUTO);
    config.setSmallFileThreshold(64 * 1024);
    config.setLargeFileThreshold(1024 * 1024);

    // 挂载多个目录
    router.mount("/static", "./public", config);
    router.mount("/downloads", "./files", config);
    router.mount("/images", "./images", config);

    // 启动服务器
    HttpServerConfig serverConfig;
    serverConfig.port = 8080;

    HttpServer server(serverConfig);
    server.start(std::move(router));

    std::cout << "Static file server running on http://0.0.0.0:8080\n";
    std::cout << "Try:\n";
    std::cout << "  http://localhost:8080/static/index.html\n";
    std::cout << "  http://localhost:8080/downloads/file.zip\n";

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}
```

### 示例2: 支持断点续传的下载客户端

```cpp
#include "galay-http/kernel/http/HttpClient.h"
#include "galay-kernel/kernel/Runtime.h"
#include <fstream>

using namespace galay::http;
using namespace galay::kernel;

Coroutine downloadWithResume(Runtime& runtime,
                             const std::string& url,
                             const std::string& outputPath) {
    HttpClient client;
    co_await client.connect(url);

    // 检查是否有未完成的下载
    size_t downloadedSize = 0;
    std::string etag;

    if (std::filesystem::exists(outputPath)) {
        downloadedSize = std::filesystem::file_size(outputPath);
        std::cout << "Found partial download: " << downloadedSize << " bytes\n";

        // 读取保存的 ETag（实际应用中应该保存到元数据文件）
        // etag = readETagFromMetadata();
    }

    // 发送 Range 请求
    std::map<std::string, std::string> headers;
    if (downloadedSize > 0) {
        headers["Range"] = "bytes=" + std::to_string(downloadedSize) + "-";
        if (!etag.empty()) {
            headers["If-Range"] = etag;
        }
    }

    while (true) {
        auto result = co_await client.get(client.url().path, headers);

        if (!result || !result.value()) continue;

        auto response = result.value().value();

        // 保存 ETag
        etag = response.header().headerPairs().getHeaderPair("ETag");

        // 打开文件
        std::ios::openmode mode = std::ios::binary;
        if (response.header().code() == HttpStatusCode::PARTIAL_CONTENT_206) {
            std::cout << "Resuming download from " << downloadedSize << " bytes\n";
            mode |= std::ios::app;  // 追加模式
        } else {
            std::cout << "Starting new download\n";
            mode |= std::ios::trunc;  // 覆盖模式
        }

        std::ofstream file(outputPath, mode);
        std::string data = response.getBodyStr();
        file.write(data.data(), data.size());
        file.close();

        std::cout << "Download complete: " << outputPath << "\n";
        break;
    }

    co_await client.close();
    co_return;
}

int main() {
    Runtime runtime(LoadBalanceStrategy::ROUND_ROBIN, 1, 1);
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    scheduler->spawn(downloadWithResume(
        runtime,
        "http://example.com/large-file.zip",
        "./large-file.zip"
    ));

    std::this_thread::sleep_for(std::chrono::seconds(10));
    runtime.stop();

    return 0;
}
```

### 示例3: 文件上传服务器

```cpp
#include "galay-http/kernel/http/HttpServer.h"
#include "galay-http/kernel/http/HttpRouter.h"
#include "galay-http/utils/Http1_1ResponseBuilder.h"
#include <filesystem>

using namespace galay::http;

Coroutine uploadHandler(HttpConn& conn, HttpRequest req) {
    // 创建上传目录
    std::filesystem::create_directories("./uploads");

    // 生成唯一文件名
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    std::string filename = "upload_" + std::to_string(timestamp);

    // 保存文件
    std::string savePath = "./uploads/" + filename;
    std::ofstream file(savePath, std::ios::binary);
    std::string content = req.getBodyStr();
    file.write(content.data(), content.size());
    file.close();

    // 返回响应
    auto response = Http1_1ResponseBuilder::created()
        .header("Location", "/downloads/" + filename)
        .json(R"({
            "status": "success",
            "filename": ")" + filename + R"(",
            "size": )" + std::to_string(content.size()) + R"(
        })")
        .build();

    auto writer = conn.getWriter();
    co_await writer.sendResponse(response);
    co_await conn.close();
    co_return;
}

int main() {
    HttpRouter router;

    // 上传接口
    router.addHandler<HttpMethod::POST>("/upload", uploadHandler);

    // 下载接口（静态文件）
    router.mount("/downloads", "./uploads");

    HttpServerConfig config;
    config.port = 8080;

    HttpServer server(config);
    server.start(std::move(router));

    std::cout << "Upload server running on http://0.0.0.0:8080\n";
    std::cout << "Upload: curl -X POST http://localhost:8080/upload --data-binary @file.zip\n";

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}
```

## 最佳实践

### 1. 选择合适的传输模式

- 小文件（<64KB）：使用 MEMORY 模式，延迟最低
- 中等文件（64KB-1MB）：使用 CHUNK 模式，平衡性能和内存
- 大文件（>1MB）：使用 SENDFILE 模式，性能最优
- 不确定：使用 AUTO 模式，自动选择

### 2. 使用 ETag 减少带宽

- 对于频繁访问的文件，使用 If-None-Match 检查是否修改
- 服务端返回 304 Not Modified 可以节省大量带宽

### 3. 实现可靠的断点续传

- 始终保存 ETag，用于验证文件完整性
- 使用 If-Range 确保文件未改变才续传
- 记录已下载的字节数，支持多次中断恢复

### 4. 错误处理

- 检查 HTTP 状态码，处理各种错误情况
- 对于网络错误，实现重试机制
- 对于 416 Range Not Satisfiable，重新开始下载

### 5. 性能优化

- 对于大文件，使用 SENDFILE 模式减少 CPU 占用
- 调整 Chunk 大小以平衡内存和性能
- 使用连接池复用 TCP 连接

## 总结

Galay-HTTP 提供了完整的文件上传下载解决方案：

- **四种传输模式**: 根据文件大小自动选择最优方式
- **Range 请求**: 完整支持 HTTP Range 协议，实现断点续传
- **ETag 验证**: 强/弱 ETag 支持，确保文件完整性
- **安全特性**: 路径遍历防护、启动时验证
- **高性能**: 零拷贝传输、SIMD 优化

通过合理使用这些特性，可以构建高性能、可靠的文件传输服务。
