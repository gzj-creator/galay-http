# Galay-HTTP

一个基于 C++23 协程的现代化异步 HTTP 服务器和客户端库，构建于 [Galay](https://github.com/galay) 异步运行时之上。

## 特性

- **协程驱动**: 完全基于 C++20/23 协程的异步 I/O 操作
- **高性能**: 零拷贝路由匹配，高效的请求/响应处理
- **易用的 API**: 简洁直观的服务器和客户端接口
- **灵活的路由系统**: 支持精确匹配、通配符和参数化路由
  - 精确匹配: `/api/users`
  - 通配符: `/static/*`
  - 参数捕获: `/user/{id}/profile`
- **Chunked 编码**: 完整支持 HTTP Chunked Transfer-Encoding
- **Builder 模式**: 方便的配置和初始化

## 依赖项

- **C++23** 编译器 (GCC 11+, Clang 14+)
- **CMake** 3.22+
- **Galay**: 核心异步运行时库
- **OpenSSL**: SSL/TLS 支持
- **pthread**: 多线程支持

## 安装

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

# 安装 (可选)
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

## 快速开始

### HTTP 服务器

```cpp
#include "galay/kernel/runtime/Runtime.h"
#include "galay-http/server/HttpServer.h"
#include "galay-http/kernel/HttpRouter.h"
#include "galay-http/utils/HttpUtils.h"

using namespace galay;
using namespace galay::http;

Coroutine<nil> handleHello(HttpRequest& request, HttpReader& reader, 
                           HttpWriter& writer, HttpParams params) {
    auto response = HttpUtils::defaultOk("txt", "Hello, World!");
    co_await writer.reply(response);
    co_return nil();
}

int main() {
    // 创建运行时
    RuntimeBuilder runtimeBuilder;
    auto runtime = runtimeBuilder.build();
    runtime.start();
    
    // 创建 HTTP 服务器
    HttpServerBuilder builder;
    HttpServer server = builder
        .addListen({"0.0.0.0", 8080})
        .threads(4)
        .build();
    
    server.listen(Host("0.0.0.0", 8080));
    
    // 配置路由
    HttpRouter router;
    router.addRoute<GET>("/hello", handleHello);
    
    // 启动服务器
    server.run(runtime, router);
    server.wait();
    server.stop();
    
    return 0;
}
```

### HTTP 客户端

```cpp
#include "galay-http/client/HttpClient.h"
#include "galay-http/utils/HttpUtils.h"

using namespace galay;
using namespace galay::http;

Coroutine<nil> makeRequest(Runtime& runtime) {
    HttpClient client(runtime);
    
    // 初始化和连接
    if (auto res = client.init(); !res) {
        std::cout << "Init failed: " << res.error().message() << std::endl;
        co_return nil();
    }
    
    if (auto res = co_await client.connect({"127.0.0.1", 8080}); !res) {
        std::cout << "Connect failed: " << res.error().message() << std::endl;
        co_return nil();
    }
    
    // 发送请求
    auto reader = client.getReader();
    auto writer = client.getWriter();
    
    HttpRequest request = HttpUtils::defaultGet("/hello");
    if (auto res = co_await writer.send(request); !res) {
        std::cout << "Send failed: " << res.error().message() << std::endl;
        co_return nil();
    }
    
    // 获取响应
    auto response = co_await reader.getResponse();
    if (response) {
        std::cout << "Response: " << response.value().getBodyStr() << std::endl;
    }
    
    co_return nil();
}

int main() {
    RuntimeBuilder builder;
    auto runtime = builder.build();
    runtime.start();
    
    runtime.schedule(makeRequest(runtime));
    
    getchar();
    runtime.stop();
    return 0;
}
```

## 路由系统

### 精确匹配

```cpp
router.addRoute<GET>("/api/users", handleUsers);
```

### 通配符路由

```cpp
// 匹配 /static/css/style.css, /static/js/app.js 等
router.addRoute<GET>("/static/*", handleStatic);

// 匹配 /endpoint/v1/app, /endpoint/v2/app 等
router.addRoute<GET>("/endpoint/*/app", handleApp);

// 通配符匹配的内容会被放入 HttpParams 中，键名为 "*"
Coroutine<nil> handleStatic(HttpRequest& request, HttpConnection& conn, 
                            HttpParams params) {
    // 对于请求 /static/css/style.css，params["*"] = "css/style.css"
    std::string path = params["*"];
    
    auto writer = conn.getResponseWriter({});
    auto response = HttpUtils::defaultOk("txt", "File: " + path);
    co_await writer.reply(response);
    co_await conn.close();
    co_return nil();
}
```

### 参数化路由

```cpp
Coroutine<nil> handleUser(HttpRequest& request, HttpReader& reader,
                          HttpWriter& writer, HttpParams params) {
    std::string userId = params["id"];  // 提取路径参数
    auto response = HttpUtils::defaultOk("txt", "User ID: " + userId);
    co_await writer.reply(response);
    co_return nil();
}

// 匹配 /user/123/profile, /user/456/profile 等
router.addRoute<GET>("/user/{id}/profile", handleUser);
```

### 批量注册路由

```cpp
HttpRouteMap routes = {
    {"/echo", handleEcho},
    {"/static/*", handleStatic},
    {"/user/{id}", handleUser},
    {"/api/{version}/data", handleData}
};

router.addRoute<GET>(routes);
```

### 多方法路由

```cpp
// 同一路径支持多个 HTTP 方法
router.addRoute<GET, POST, PUT>("/api/resource", handleResource);
```

## Chunked Transfer-Encoding

galay-http 完整支持 HTTP Chunked Transfer Encoding (RFC 7230)。详细文档请参考 [docs/03-http_chunked.md](docs/03-http_chunked.md)。

### 服务器端接收 Chunked 请求

```cpp
// 1. 读取请求头
HttpRequest request;
auto result = co_await reader.getRequest(request);

// 2. 检查是否是 chunked 编码
if (result.value() == false && request.header().isChunked()) {
    std::string allData;
    bool isLast = false;

    // 3. 循环读取所有 chunk
    while (!isLast) {
        auto chunkResult = co_await reader.getChunk(allData);
        if (!chunkResult) {
            // 错误处理
            break;
        }
        isLast = chunkResult.value();
        // allData 包含所有已接收的 chunk 数据（追加方式）
    }

    LogInfo("Received all chunks: {} bytes", allData.size());
}
```

### 服务器端发送 Chunked 响应

```cpp
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
std::string empty;
co_await writer.sendChunk(empty, true);
```

### 客户端发送 Chunked 请求

```cpp
// 1. 发送请求头
HttpRequestHeader header;
header.method() = HttpMethod::HttpMethod_Post;
header.uri() = "/upload";
header.headerPairs().addHeaderPair("Transfer-Encoding", "chunked");
co_await writer.sendHeader(std::move(header));

// 2. 发送数据块
co_await writer.sendChunk("chunk1", false);
co_await writer.sendChunk("chunk2", false);

// 3. 发送最后一个 chunk
std::string empty;
co_await writer.sendChunk(empty, true);
```

### 性能数据

- **编码性能**: 400万 ops/sec (0.25 μs/op)
- **解析性能**: 217万 ops/sec (0.46 μs/op)
- **RingBuffer 集成**: 96万 ops/sec (1.04 μs/op)

### 客户端接收 Chunked 响应

```cpp
Coroutine<nil> receiveChunked(HttpClient& client) {
    auto reader = client.getReader();
    
    auto response = co_await reader.getResponse();
    if (response && response.value().header().isChunked()) {
        co_await reader.getChunkData([](std::string chunk) {
            std::cout << "Received chunk: " << chunk << std::endl;
        });
    }
    
    co_return nil();
}
```

## HttpSettings 配置

```cpp
HttpSettings settings {
    .maxHeaderSize = 8192,              // 最大请求头大小
    .maxBodySize = 10 * 1024 * 1024,    // 最大请求体大小 (10MB)
    .timeout = std::chrono::seconds(30)  // 超时时间
};

HttpClient client(runtime, settings);
// 或
server.run(runtime, router, settings);
```

## 项目结构

```
galay-http/
├── galay-http/
│   ├── client/          # HTTP 客户端实现
│   ├── server/          # HTTP 服务器实现
│   ├── kernel/          # 核心组件
│   │   ├── HttpConnection.h/cc   # 连接管理
│   │   ├── HttpRouter.h/cc       # 路由系统
│   │   ├── HttpReader.h/cc       # 请求读取
│   │   └── HttpWriter.h/cc       # 响应写入
│   ├── protoc/          # HTTP 协议
│   │   ├── HttpRequest.h/cc      # HTTP 请求
│   │   ├── HttpResponse.h/cc     # HTTP 响应
│   │   ├── HttpHeader.h/cc       # HTTP 头部
│   │   └── HttpBody.h/cc         # HTTP 主体
│   └── utils/           # 工具类
│       ├── HttpLogger.h/cc       # 日志
│       └── HttpUtils.h/cc        # 实用函数
└── test/                # 测试用例
    ├── test_http_server.cc
    ├── test_http_client.cc
    ├── test_chunk_server.cc
    └── test_chunk_client.cc
```

## 编译测试

```bash
cd build
make

# 运行服务器测试
./test/test_http_server

# 在另一个终端运行客户端测试
./test/test_http_client

# 测试 chunked 编码
./test/test_chunk_server
./test/test_chunk_client
```

## API 工具类

### HttpUtils

```cpp
// 创建默认响应
auto response = HttpUtils::defaultOk("txt", "Response body");
auto jsonResponse = HttpUtils::defaultOk("json", jsonString);

// 创建默认请求
auto request = HttpUtils::defaultGet("/path");
auto postRequest = HttpUtils::defaultPost("/api", "request body");
```

### HttpLogger

```cpp
// 获取日志实例并设置日志级别
HttpLogger::getInstance()
    ->getLogger()
    ->getSpdlogger()
    ->set_level(spdlog::level::debug);
```

## 性能特性

- **零拷贝路由匹配**: 直接在原字符串上进行路径匹配，避免创建临时字符串
- **高效内存管理**: 使用移动语义和右值引用减少不必要的拷贝
- **协程调度**: 基于 Galay 运行时的高效协程调度器
- **多线程支持**: 可配置的工作线程数

## 示例场景

### RESTful API 服务器

```cpp
HttpRouteMap apiRoutes = {
    {"/api/users", handleListUsers},
    {"/api/user/{id}", handleGetUser},
    {"/api/user/{id}/posts", handleUserPosts}
};

router.addRoute<GET>(apiRoutes);

HttpRouteMap postRoutes = {
    {"/api/user", handleCreateUser}
};
router.addRoute<POST>(postRoutes);
```

### 静态文件服务器

使用 `mount()` 方法可以轻松挂载静态文件目录：

```cpp
HttpRouter router;

// 挂载静态文件目录
// 注意：mount() 会立即验证路径，不存在会抛出异常
try {
    // GET /static/css/style.css -> 读取 ./public/css/style.css
    router.mount("/static", "./public");
    
    // 挂载多个目录
    router.mount("/assets", "./assets");
    router.mount("/images", "./images");
} catch (const std::runtime_error& e) {
    std::cerr << "Mount failed: " << e.what() << std::endl;
    return 1;
}

server.run(runtime, router);
```

**安全特性：**
- ✅ 启动时验证路径（早期发现配置错误）
- ✅ 自动防止路径遍历攻击（例如 `../../../etc/passwd`）
- ✅ 只允许访问指定目录下的文件
- ✅ 自动处理路径规范化
- ✅ 根据文件扩展名自动设置正确的 Content-Type

**支持的文件类型：**
- HTML/CSS/JavaScript
- 图片（PNG, JPEG, GIF, SVG）
- JSON/XML
- PDF
- 纯文本
- 其他（默认为 application/octet-stream）

**完整示例：**

```cpp
#include "galay/kernel/runtime/Runtime.h"
#include "galay-http/server/HttpServer.h"
#include "galay-http/kernel/HttpRouter.h"

using namespace galay;
using namespace galay::http;

int main() {
    RuntimeBuilder runtimeBuilder;
    auto runtime = runtimeBuilder.build();
    runtime.start();
    
    HttpServerBuilder builder;
    HttpServer server = builder.build();
    server.listen(Host("0.0.0.0", 8080));
    
    HttpRouter router;
    router.mount("/static", "./public");
    
    server.run(runtime, router);
    server.wait();
    server.stop();
    
    return 0;
}
```

## 许可证

[指定你的许可证]

## 贡献

欢迎提交 Issue 和 Pull Request！

## 联系方式

- 项目主页: [GitHub](https://github.com/your-org/galay-http)
- Issue 追踪: [GitHub Issues](https://github.com/your-org/galay-http/issues)

## 致谢

本项目基于 [Galay](https://github.com/galay) 异步运行时框架开发。

