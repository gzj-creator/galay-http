# HTTPS/SSL 支持

## 状态：已完成 ✅

## 概述
通过模板特化支持 TcpSocket（使用 readv）和 SslSocket（使用 recv），实现 HTTP 和 HTTPS 双协议支持。

## 已完成功能

### CMake 配置
- [x] `cmake/options.cmake` - SSL 编译选项
- [x] `GALAY_HTTP_ENABLE_SSL` 选项（默认 OFF）
- [x] 自动查找并链接 `galay-ssl` 库
- [x] `GALAY_HTTP_SSL_ENABLED` 编译宏

### 内核层模板化
- [x] `HttpConnImpl<SocketType>` - HTTP 连接模板类
- [x] `HttpReaderImpl<SocketType>` - HTTP 读取器模板类（使用模板特化适配 SslSocket）
- [x] `HttpWriterImpl<SocketType>` - HTTP 写入器模板类
- [x] `HttpServerImpl<SocketType>` - HTTP 服务器模板类
- [x] `HttpsServer` - HTTPS 服务器类（包含 SSL 上下文管理和异步握手）
- [x] `HttpsClient` - HTTPS 客户端类（继承自 HttpClientImpl，包含 SSL 上下文管理、SNI 支持和异步握手）
- [x] WebSocket 相关类模板化

### 类型别名
```cpp
// HTTP (TcpSocket)
using HttpConn = HttpConnImpl<TcpSocket>;
using HttpReader = HttpReaderImpl<TcpSocket>;
using HttpWriter = HttpWriterImpl<TcpSocket>;
using HttpServer = HttpServerImpl<TcpSocket>;
using HttpClient = HttpClientImpl<TcpSocket>;

// HTTPS (SslSocket) - 仅 GALAY_HTTP_SSL_ENABLED 时可用
using HttpsConn = HttpConnImpl<galay::ssl::SslSocket>;
using HttpsReader = HttpReaderImpl<galay::ssl::SslSocket>;
using HttpsWriter = HttpWriterImpl<galay::ssl::SslSocket>;
// HttpsServer 和 HttpsClient 是独立的类，包含 SSL 上下文管理
```

### 技术实现
- 使用 `is_ssl_socket` 类型特征检测 Socket 类型
- 模板特化：TcpSocket 使用 `readv`，SslSocket 使用 `recv`
- 异步 SSL 握手，支持 `WantRead/WantWrite` 重试
- 循环读取/发送模式确保数据完整传输
- **HttpsClient 继承架构**：继承自 `HttpClientImpl<SslSocket>`，消除代码重复
- **SNI 支持**：自动设置 Server Name Indication
- **SSL 重试处理**：正确处理 `SSL_ERROR_WANT_READ/WRITE` 状态
- **Keep-Alive 支持**：服务器和客户端均支持连接复用

## 使用方法

### 编译启用 SSL
```bash
cmake .. -DGALAY_HTTP_ENABLE_SSL=ON
make
```

### HTTPS 服务器示例 (Keep-Alive)
```cpp
#include "galay-http/kernel/http/HttpServer.h"

using namespace galay::http;

Coroutine handler(HttpConnImpl<galay::ssl::SslSocket> conn) {
    auto reader = conn.getReader();
    auto writer = conn.getWriter();

    while (true) {  // Keep-Alive 循环
        HttpRequest request;

        // 读取请求
        while (true) {
            auto result = co_await reader.getRequest(request);
            if (!result) {
                co_await conn.close();
                co_return;
            }
            if (result.value()) break;
        }

        // 检查 Connection 头
        bool keep_alive = request.header().headerPairs().getValue("Connection") != "close";

        auto response = Http1_1ResponseBuilder::ok()
            .header("Connection", keep_alive ? "keep-alive" : "close")
            .text("Hello HTTPS!")
            .build();

        // 发送响应
        while (true) {
            auto result = co_await writer.sendResponse(response);
            if (!result || result.value()) break;
        }

        if (!keep_alive) break;
    }

    co_await conn.close();
}

int main() {
    HttpsServerConfig config;
    config.port = 8443;
    config.cert_path = "server.crt";
    config.key_path = "server.key";

    HttpsServer server(config);
    server.start(handler);
    // ...
}
```

### HTTPS 客户端示例 (Keep-Alive)
```cpp
#include "galay-http/kernel/http/HttpClient.h"

Coroutine testHttpsClient() {
    HttpsClientConfig config;
    config.verify_peer = false;  // 测试时不验证证书

    HttpsClient client(config);

    // 连接
    co_await client.connect("https://localhost:8443/");

    // SSL 握手
    while (!client.isHandshakeCompleted()) {
        auto result = co_await client.handshake();
        if (!result) {
            auto& err = result.error();
            if (err.code() == galay::ssl::SslErrorCode::kHandshakeWantRead ||
                err.code() == galay::ssl::SslErrorCode::kHandshakeWantWrite) {
                continue;
            }
            co_return;  // 握手失败
        }
        break;
    }

    auto& writer = client.getWriter();
    auto& reader = client.getReader();

    // 在同一连接上发送多个请求 (Keep-Alive)
    for (int i = 0; i < 100; i++) {
        HttpRequest request;
        HttpRequestHeader header;
        header.method() = HttpMethod::GET;
        header.uri() = "/";
        header.version() = HttpVersion::HttpVersion_1_1;
        header.headerPairs().addHeaderPair("Host", "localhost");
        header.headerPairs().addHeaderPair("Connection", "keep-alive");
        request.setHeader(std::move(header));

        // 发送请求
        while (true) {
            auto result = co_await writer.sendRequest(request);
            if (!result || result.value()) break;
        }

        // 接收响应
        HttpResponse response;
        while (true) {
            auto result = co_await reader.getResponse(response);
            if (!result || result.value()) break;
        }
    }

    co_await client.close();
}
```

## 测试结果

### 功能测试
- [x] HTTPS 服务器启动成功
- [x] SSL 证书加载成功
- [x] SSL 握手成功
- [x] 请求/响应正常
- [x] HTTPS 客户端连接成功
- [x] HTTPS 客户端收发数据正常
- [x] Keep-Alive 连接复用正常
- [x] SNI 设置正常

### 压测结果 (Keep-Alive 连接复用)

| 测试场景 | 请求数 | 连接数 | 成功率 | QPS |
|---------|--------|--------|--------|-----|
| 单连接 100 请求 | 100 | 1 | 100% | **1,724** |
| 10 连接各 100 请求 | 1,000 | 10 | 100% | **17,241** |
| 20 连接各 100 请求 | 2,000 | 20 | 100% | **29,412** |
| 50 连接各 100 请求 | 5,000 | 50 | 100% | **69,444** |
| 100 连接各 100 请求 | 10,000 | 100 | 100% | **82,645** |

**测试环境：** macOS, 8 IO调度器, 4 客户端调度器, TLS 1.3

### 性能对比 (短连接 vs Keep-Alive)

| 场景 | 短连接 QPS | Keep-Alive QPS | 提升倍数 |
|------|-----------|----------------|----------|
| 单连接 | 35 | 1,724 | **49x** |
| 10 并发 | 412 | 17,241 | **42x** |
| 20 并发 | 1,182 | 29,412 | **25x** |
| 50 并发 | 2,950 | 69,444 | **24x** |

## 修复记录

### 2026-01-31 修复
1. **HttpsClient 架构重构**：从独立类改为继承 `HttpClientImpl<SslSocket>`，消除危险的 `reinterpret_cast`
2. **SslSocket 构造修复**：移除错误的 `IPType::IPV4` 参数
3. **SNI 支持**：添加 `setHostname()` 调用
4. **SSL 读取重试**：正确处理 `SSL_ERROR_WANT_READ/WRITE` 状态，返回 false 继续读取而非报错
5. **Keep-Alive 支持**：服务器和客户端均支持连接复用

## 依赖
- `galay-ssl` 库（提供 `SslSocket` 和 `SslContext`）
- OpenSSL

## 文件变更
- `cmake/options.cmake` - SSL 编译选项
- `galay-http/kernel/http/HttpReader.h` - 模板特化支持 SslSocket，SSL 重试处理
- `galay-http/kernel/http/HttpClient.h` - HttpsClient 继承架构重构，SNI 支持
- `galay-http/kernel/http/HttpServer.h` - HttpsServer 实现
- `test/T21-HttpsServer.cc` - HTTPS 服务器测试（支持 Keep-Alive）
- `test/T22-HttpsClient.cc` - HTTPS 客户端测试
- `test/T23-HttpsStressTest.cc` - HTTPS 压力测试（Keep-Alive 连接复用）
