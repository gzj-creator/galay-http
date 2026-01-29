# Galay-HTTP

一个基于 C++20/23 协程的现代化高性能异步 HTTP/WebSocket 库，构建于 [Galay](https://github.com/galay) 异步运行时之上。

## 核心特性

### 协程驱动的异步架构
- **完全异步**: 基于 C++20/23 协程的非阻塞 I/O 操作
- **高性能调度**: 内置 Runtime 管理，支持多 IO 调度器和计算调度器
- **超时支持**: 所有异步操作均支持超时设置

### HTTP 服务器
- **内置 Runtime**: 服务器自带运行时管理，无需手动配置调度器
- **灵活的路由系统**: 混合策略（精确匹配 O(1) + Trie 树模糊匹配 O(k)）
  - 精确路径: `/api/users`
  - 路径参数: `/user/{id}`
  - 通配符: `/static/*`
  - 贪婪通配符: `/files/**`
- **静态文件服务**: 支持多种传输模式、Range 请求、ETag 缓存验证
- **安全特性**: 路径遍历攻击防护、启动时路径验证

### HTTP 客户端
- **URL 连接支持**: 直接使用 URL 字符串连接服务器
- **HttpClientAwaitable**: 自动处理请求发送和响应接收的完整流程
- **循环等待机制**: 自动处理不完整的数据传输
- **超时控制**: 支持请求级别的超时设置

### 文件传输和断点续传
- **四种传输模式**:
  - MEMORY: 内存模式（小文件 <64KB）
  - CHUNK: 分块模式（中等文件 64KB-1MB）
  - SENDFILE: 零拷贝模式（大文件 >1MB）
  - AUTO: 自动选择最优模式
- **Range 请求**: 完整支持 HTTP Range 协议
  - 单范围: `bytes=0-499`
  - 多范围: `bytes=0-99,200-299`
  - 后缀范围: `bytes=500-`
  - 前缀范围: `bytes=-500`
- **ETag 支持**: 强/弱 ETag 生成和验证，支持 If-None-Match、If-Match、If-Range
- **断点续传**: 基于 Range 和 ETag 的可靠断点续传机制

### WebSocket 支持
- **完整的 WebSocket 协议**: RFC 6455 标准实现
- **帧类型支持**: Text、Binary、Ping、Pong、Close
- **心跳机制**: 自动心跳保活
- **协议升级**: 从 HTTP 无缝升级到 WebSocket

### 高性能特性
- **零拷贝路由匹配**: 直接在原字符串上进行路径匹配
- **SIMD 优化**: 路由匹配支持 SIMD 加速
- **高效内存管理**: 移动语义和右值引用减少拷贝
- **Chunked 编码**: 高性能实现（编码 400万 ops/sec，解析 217万 ops/sec）

### 易用的 API
- **Builder 模式**: 链式调用构造 HTTP 请求和响应
- **自动路由分发**: HttpServer 内置 Router 支持
- **简洁的协程接口**: 使用 `co_await` 进行异步操作

## 快速开始

### HTTP 服务器示例

```cpp
#include "galay-http/kernel/http/HttpServer.h"
#include "galay-http/kernel/http/HttpRouter.h"
#include "galay-http/utils/Http1_1ResponseBuilder.h"

using namespace galay::http;
using namespace galay::kernel;

// Echo 处理器
Coroutine echoHandler(HttpConn& conn, HttpRequest req) {
    std::string body = req.getBodyStr();

    // 使用 Builder 构造响应
    auto response = Http1_1ResponseBuilder::ok()
        .header("Server", "Galay-HTTP/1.0")
        .text("Echo: " + body)
        .build();

    auto writer = conn.getWriter();
    co_await writer.sendResponse(response);
    co_await conn.close();
    co_return;
}

int main() {
    // 创建路由器
    HttpRouter router;
    router.addHandler<HttpMethod::POST>("/echo", echoHandler);

    // 配置并启动服务器
    HttpServerConfig config;
    config.host = "0.0.0.0";
    config.port = 8080;

    HttpServer server(config);
    server.start(std::move(router));  // 服务器内置 Runtime

    // 保持运行
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}
```

### HTTP 客户端示例

```cpp
#include "galay-http/kernel/http/HttpClient.h"
#include "galay-kernel/kernel/Runtime.h"

using namespace galay::http;
using namespace galay::kernel;

Coroutine sendRequest(Runtime& runtime) {
    // 创建客户端并连接
    HttpClient client;
    auto connect_result = co_await client.connect("http://127.0.0.1:8080/echo");

    if (!connect_result) {
        std::cerr << "Connection failed\n";
        co_return;
    }

    // 发送 POST 请求并接收响应（自动循环等待）
    while (true) {
        auto result = co_await client.post(
            client.url().path,
            "Hello, Server!",
            "text/plain"
        );

        if (!result) {
            std::cerr << "Request failed\n";
            co_return;
        }

        // 检查是否完成
        if (!result.value()) {
            continue;  // 继续等待
        }

        // 获取响应
        auto response = result.value().value();
        std::cout << "Response: " << response.getBodyStr() << "\n";
        break;
    }

    co_await client.close();
    co_return;
}

int main() {
    Runtime runtime(LoadBalanceStrategy::ROUND_ROBIN, 1, 1);
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    scheduler->spawn(sendRequest(runtime));

    std::this_thread::sleep_for(std::chrono::seconds(3));
    runtime.stop();

    return 0;
}
```

## 核心组件

### HttpServer

HttpServer 是一个功能完整的 HTTP 服务器，内置 Runtime 管理调度器。

**特性**:
- 内置 Runtime，自动管理 IO 和计算调度器
- 支持两种启动方式：直接处理器或 HttpRouter
- 自动处理连接接受和请求分发
- 可配置的主机、端口、调度器数量等

**配置选项**:
```cpp
HttpServerConfig config;
config.host = "0.0.0.0";
config.port = 8080;
config.backlog = 128;
config.io_scheduler_count = 0;      // 0 = 自动（2 * CPU 核心数）
config.compute_scheduler_count = 0; // 0 = 自动（CPU 核心数）
```

### HttpClient

HttpClient 提供异步 HTTP 客户端功能，支持 URL 连接和自动请求/响应流程。

**特性**:
- URL 解析和连接：`client.connect("http://example.com:8080/path")`
- HttpClientAwaitable 等待体：自动处理请求发送和响应接收
- 支持所有 HTTP 方法：GET、POST、PUT、DELETE、PATCH、HEAD、OPTIONS、TRACE、CONNECT
- 超时支持：`co_await client.get("/api").timeout(std::chrono::seconds(5))`

**示例**:
```cpp
HttpClient client;
co_await client.connect("http://example.com/api");

// 发送 GET 请求
while (true) {
    auto result = co_await client.get("/users");
    if (!result) break;
    if (!result.value()) continue;  // 继续等待

    auto response = result.value().value();
    // 处理响应
    break;
}
```

### HttpRouter

HttpRouter 是一个高性能的路由系统，采用混合策略实现快速路由匹配。

**路由模式**:
```cpp
HttpRouter router;

// 精确路径
router.addHandler<HttpMethod::GET>("/api/users", handleUsers);

// 路径参数
router.addHandler<HttpMethod::GET>("/user/{id}", handleUser);

// 通配符
router.addHandler<HttpMethod::GET>("/static/*", handleStatic);

// 贪婪通配符
router.addHandler<HttpMethod::GET>("/files/**", handleFiles);

// 多方法支持
router.addHandler<HttpMethod::GET, HttpMethod::POST>("/api/resource", handleResource);
```

**静态文件挂载**:
```cpp
// mount() - 启动时验证路径
router.mount("/static", "./public");

// mountHardly() - 预加载文件到内存（适合小文件）
router.mountHardly("/assets", "./assets");
```

## 文件上传下载和断点续传

### 静态文件传输模式

Galay-HTTP 支持四种文件传输模式，可根据文件大小自动选择最优方式：

```cpp
StaticFileConfig config;

// 设置传输模式
config.setTransferMode(FileTransferMode::AUTO);  // 自动选择

// 自定义阈值
config.setSmallFileThreshold(64 * 1024);    // 64KB
config.setLargeFileThreshold(1024 * 1024);  // 1MB

// 挂载时使用配置
router.mount("/files", "./files", config);
```

**传输模式说明**:
- **MEMORY**: 小文件完整读入内存（<64KB），简单高效
- **CHUNK**: HTTP chunked 编码分块传输（64KB-1MB），内存占用可控
- **SENDFILE**: 零拷贝 sendfile 系统调用（>1MB），性能最优
- **AUTO**: 根据文件大小自动选择（推荐）

### Range 请求和断点续传

**服务端支持 Range 请求**:

Range 请求由 HttpRouter 的静态文件挂载自动处理，无需额外代码。

**客户端发送 Range 请求**:
```cpp
HttpClient client;
co_await client.connect("http://example.com/large-file.zip");

// 请求文件的一部分（断点续传）
while (true) {
    auto result = co_await client.get("/large-file.zip", {
        {"Range", "bytes=1024000-"},  // 从 1MB 处继续下载
        {"If-Range", etag}            // 验证文件未改变
    });

    if (!result || !result.value()) continue;

    auto response = result.value().value();

    // 检查是否是 206 Partial Content
    if (response.header().code() == HttpStatusCode::PARTIAL_CONTENT_206) {
        // 获取 Content-Range 头
        auto contentRange = response.header().headerPairs().getHeaderPair("Content-Range");
        std::cout << "Received: " << contentRange << "\n";

        // 保存数据
        std::string data = response.getBodyStr();
        // ... 追加到文件
    }
    break;
}
```

**支持的 Range 格式**:
- `bytes=0-499` - 单范围（前 500 字节）
- `bytes=0-99,200-299` - 多范围
- `bytes=500-` - 后缀范围（从 500 字节到文件末尾）
- `bytes=-500` - 前缀范围（最后 500 字节）

### ETag 缓存验证

ETag 用于验证文件是否被修改，支持断点续传的可靠性。

**服务端自动生成 ETag**:

静态文件挂载会自动为每个文件生成 ETag（基于 inode + mtime + size）。

**客户端使用 ETag**:
```cpp
// 首次请求，获取 ETag
auto response1 = co_await client.get("/file.zip");
std::string etag = response1.value().value()
    .header().headerPairs().getHeaderPair("ETag");

// 后续请求，使用 If-None-Match 检查是否修改
auto response2 = co_await client.get("/file.zip", {
    {"If-None-Match", etag}
});

// 如果返回 304 Not Modified，说明文件未改变
if (response2.value().value().header().code() == HttpStatusCode::NOT_MODIFIED_304) {
    std::cout << "File not modified, use cached version\n";
}
```

**断点续传中使用 If-Range**:
```cpp
// If-Range 确保文件未改变才使用 Range 请求
auto result = co_await client.get("/file.zip", {
    {"Range", "bytes=1024000-"},
    {"If-Range", etag}  // 如果 ETag 不匹配，服务器返回完整文件
});
```

## Builder 模式 API

### Http1_1ResponseBuilder

使用 Builder 模式构造 HTTP 响应：

```cpp
// 基本用法
auto response = Http1_1ResponseBuilder::ok()
    .header("Server", "Galay-HTTP/1.0")
    .text("Hello, World!")
    .build();

// JSON 响应
auto jsonResponse = Http1_1ResponseBuilder::ok()
    .json(R"({"status": "ok", "data": [1, 2, 3]})")
    .build();

// HTML 响应
auto htmlResponse = Http1_1ResponseBuilder::ok()
    .html("<h1>Welcome</h1>")
    .build();

// 自定义状态码
auto customResponse = Http1_1ResponseBuilder()
    .status(201)
    .header("Location", "/users/123")
    .json(R"({"id": 123})")
    .build();
```

**快捷方法**:
- `ok()` - 200 OK
- `created()` - 201 Created
- `noContent()` - 204 No Content
- `badRequest()` - 400 Bad Request
- `unauthorized()` - 401 Unauthorized
- `forbidden()` - 403 Forbidden
- `notFound()` - 404 Not Found
- `internalServerError()` - 500 Internal Server Error

### Http1_1RequestBuilder

使用 Builder 模式构造 HTTP 请求：

```cpp
// GET 请求
auto getRequest = Http1_1RequestBuilder::get("/api/users")
    .host("example.com")
    .header("User-Agent", "Galay-HTTP-Client/1.0")
    .build();

// POST JSON 请求
auto postRequest = Http1_1RequestBuilder::post("/api/users")
    .host("example.com")
    .json(R"({"name": "John", "age": 30})")
    .build();

// POST 表单请求
auto formRequest = Http1_1RequestBuilder::post("/login")
    .host("example.com")
    .form({
        {"username", "john"},
        {"password", "secret"}
    })
    .build();
```

## Chunked Transfer-Encoding

Galay-HTTP 完整支持 HTTP Chunked Transfer Encoding (RFC 7230)。

### 服务器端发送 Chunked 响应

```cpp
Coroutine handleChunked(HttpConn& conn, HttpRequest req) {
    auto writer = conn.getWriter();

    // 1. 发送响应头
    HttpResponseHeader header;
    header.version() = HttpVersion::HttpVersion_1_1;
    header.code() = HttpStatusCode::OK_200;
    header.headerPairs().addHeaderPair("Transfer-Encoding", "chunked");
    co_await writer.sendHeader(std::move(header));

    // 2. 发送多个 chunk
    co_await writer.sendChunk("First chunk\n", false);
    co_await writer.sendChunk("Second chunk\n", false);
    co_await writer.sendChunk("Third chunk\n", false);

    // 3. 发送最后一个 chunk
    co_await writer.sendChunk("", true);

    co_await conn.close();
    co_return;
}
```

### 客户端接收 Chunked 响应

```cpp
auto reader = client.getReader();
HttpResponse response;
co_await reader.getResponse(response);

if (response.header().isChunked()) {
    std::string allData;
    bool isLast = false;

    while (!isLast) {
        auto chunkResult = co_await reader.getChunk(allData);
        if (!chunkResult) break;
        isLast = chunkResult.value();
        // allData 包含所有已接收的数据
    }
}
```

## WebSocket 支持

### WebSocket 服务器

```cpp
#include "galay-http/kernel/websocket/WsConn.h"

Coroutine wsHandler(HttpConn& conn, HttpRequest req) {
    // 升级到 WebSocket
    auto wsConn = co_await galay::websocket::upgradeToWebSocket(conn, req);

    if (!wsConn) {
        co_return;
    }

    // 接收和发送消息
    while (true) {
        WebSocketFrame frame;
        auto result = co_await wsConn->receive(frame);

        if (!result || frame.opcode == WebSocketOpcode::CLOSE) {
            break;
        }

        // Echo 消息
        co_await wsConn->send(frame.payload, WebSocketOpcode::TEXT);
    }

    co_await wsConn->close();
    co_return;
}
```

### WebSocket 客户端

```cpp
#include "galay-http/kernel/websocket/WsClient.h"

Coroutine wsClient() {
    WsClient client;
    co_await client.connect("ws://127.0.0.1:8080/ws");

    // 发送消息
    co_await client.send("Hello, WebSocket!", WebSocketOpcode::TEXT);

    // 接收消息
    WebSocketFrame frame;
    co_await client.receive(frame);

    std::cout << "Received: " << frame.payload << "\n";

    co_await client.close();
    co_return;
}
```

## 性能数据

- **HTTP 解析**: ~0.5 μs/op
- **Chunk 编码**: 0.25 μs/op (400万 ops/sec)
- **Chunk 解析**: 0.46 μs/op (217万 ops/sec)
- **路由匹配**: O(1) 精确匹配，O(k) 模糊匹配
- **零拷贝传输**: sendfile 系统调用，CPU 占用低

## 依赖项

- **C++20/23** 编译器 (GCC 11+, Clang 14+)
- **CMake** 3.22+
- **Galay**: 核心异步运行时库
- **OpenSSL**: SSL/TLS 支持（可选）
- **pthread**: 多线程支持

## 构建和安装

### 从源码构建

```bash
# 克隆仓库
git clone https://github.com/your-org/galay-http.git
cd galay-http

# 创建构建目录
mkdir build && cd build

# 配置和编译
cmake ..
make -j$(nproc)

# 运行测试
ctest

# 安装（可选）
sudo make install
```

### CMake 集成

在你的项目的 `CMakeLists.txt` 中:

```cmake
find_package(galay-http REQUIRED)

target_link_libraries(your_target
    PRIVATE galay-http
)
```

## 示例程序

项目包含多个示例程序，位于 `example/` 目录：

- `1.echo_server.cc` - Echo 服务器示例
- `2.echo_client.cc` - Echo 客户端示例
- `3.websocket_server.cc` - WebSocket 服务器示例
- `4.websocket_client.cc` - WebSocket 客户端示例

运行示例：
```bash
cd build
./example/1.echo_server 8080
# 在另一个终端
./example/2.echo_client http://127.0.0.1:8080/echo "Hello"
```

## 测试

项目包含 20+ 个测试用例，覆盖所有核心功能：

```bash
cd build
ctest --verbose
```

测试包括：
- HTTP 解析和协议测试
- Chunked 编码/解码测试
- 路由系统测试
- Range 和 ETag 测试
- 静态文件传输模式测试
- WebSocket 协议测试
- 超时机制测试

## 文档

详细文档位于 `docs/` 目录：

- `01-http_parser.md` - HTTP 解析器实现
- `02-http_reader_writer.md` - Reader/Writer 异步 I/O
- `03-http_chunked.md` - Chunked 编码完整指南
- `04-websocket.md` - WebSocket 协议实现
- `05-timeout_support.md` - 超时机制
- `07-static_file_transfer_modes.md` - 文件传输模式详解
- `11-HttpRouter.md` - 路由系统文档
- `12-HttpRouterCompleteness.md` - 路由系统完整性文档

## 安全特性

- **路径遍历防护**: 自动检测和阻止 `../` 攻击
- **启动时验证**: mount() 在启动时验证路径，早期发现配置错误
- **路径规范化**: 自动处理路径规范化
- **Content-Type 自动设置**: 根据文件扩展名自动设置正确的 MIME 类型

## 许可证

[指定你的许可证]

## 贡献

欢迎提交 Issue 和 Pull Request！

## 联系方式

- 项目主页: [GitHub](https://github.com/your-org/galay-http)
- Issue 追踪: [GitHub Issues](https://github.com/your-org/galay-http/issues)

## 致谢

本项目基于 [Galay](https://github.com/galay) 异步运行时框架开发。
