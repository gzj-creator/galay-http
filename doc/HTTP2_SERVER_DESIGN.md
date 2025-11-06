# HTTP/2 Server 架构设计

## 概述

galay-http 框架提供了独立的 `Http2Server` 类来专门处理 HTTP/2 over TLS (h2) 连接。

## 架构分离

### 之前的设计

之前 HTTP/2 支持混合在 `HttpsServer` 中：
- `HttpsServer` 需要同时处理 HTTP/1.1 和 HTTP/2
- 代码复杂，职责不清晰
- 需要在运行时检测协议并分发到不同的处理器

### 新的设计

现在 HTTP/2 有了独立的服务器类：

```
HttpServer       - 专门处理 HTTP/1.x (无加密)
HttpsServer      - 专门处理 HTTPS + HTTP/1.x  
Http2Server      - 专门处理 HTTP/2 over TLS (h2)
```

### 优势

1. **职责单一**：每个服务器类只处理一种协议
2. **代码清晰**：不需要复杂的协议检测和分发逻辑
3. **易于维护**：HTTP/2 的改动不会影响 HTTP/1.1
4. **性能优化**：可以针对 HTTP/2 做专门的优化

## Http2Server 类设计

### 类结构

```cpp
class Http2Server
{
public:
    // 构造
    Http2Server(TcpSslServer&& server, const std::string& cert, const std::string& key);
    
    // 运行服务器（使用回调处理帧）
    void run(Runtime& runtime, 
            const Http2Callbacks& callbacks,
            Http2Settings params = Http2Settings());
    
    // 运行服务器（使用自定义连接处理器）
    void run(Runtime& runtime, const Http2ConnFunc& handler);
    
    void stop();
    void wait();
    
private:
    void configureAlpn();  // 自动配置 ALPN 为 h2 only
    Coroutine<nil> handleConnection(...);
    Coroutine<nil> processHttp2Frames(...);
};
```

### 自动 ALPN 配置

`Http2Server` 会自动配置 ALPN 为**仅支持 h2**：

```cpp
void Http2Server::configureAlpn()
{
    SSL_CTX* ctx = m_server.getSSLContext();
    AlpnProtocolList alpn_list = AlpnProtocolList::http2Only();
    configureServerAlpn(ctx, alpn_list);
}
```

这意味着：
- 客户端必须支持 HTTP/2
- 不支持 HTTP/2 的客户端会协商失败
- 服务器不会降级到 HTTP/1.1

### 使用示例

#### 基本用法

```cpp
#include "galay-http/server/Http2Server.h"

// 创建服务器
Http2Server server = Http2ServerBuilder("server.crt", "server.key")
                        .addListen(Host("0.0.0.0", 8443))
                        .build();

// 设置回调
Http2Callbacks callbacks;
callbacks.on_headers = onHeaders;
callbacks.on_data = onData;
callbacks.on_error = onError;

// 运行
server.run(runtime, callbacks);
server.wait();
```

#### 回调实现

```cpp
Coroutine<nil> onHeaders(Http2Connection& conn,
                         uint32_t stream_id,
                         const std::map<std::string, std::string>& headers,
                         bool end_stream)
{
    // 处理 HEADERS 帧
    std::string method = headers.at(":method");
    std::string path = headers.at(":path");
    
    // ... 业务逻辑 ...
    
    co_return nil();
}

Coroutine<nil> onData(Http2Connection& conn,
                      uint32_t stream_id,
                      const std::string& data,
                      bool end_stream)
{
    // 处理 DATA 帧
    if (end_stream) {
        // 数据接收完成，发送响应
        auto writer = conn.getWriter({});
        co_await writer.sendHeaders(...);
        co_await writer.sendData(...);
    }
    
    co_return nil();
}
```

## 与 HttpsServer 的对比

| 特性 | HttpsServer | Http2Server |
|------|-------------|-------------|
| 支持协议 | HTTP/1.x over TLS | HTTP/2 over TLS (h2) |
| ALPN 配置 | h2, http/1.1 (可选) | h2 only |
| 协议降级 | 支持 | 不支持 |
| API 风格 | Router + Handler | Callbacks |
| 适用场景 | 传统 HTTPS 服务 | 高性能 HTTP/2 服务 |

## 何时使用

### 使用 HttpsServer

- 需要兼容不支持 HTTP/2 的客户端
- 传统的 REST API 服务
- 使用路由和处理器的编程模型

### 使用 Http2Server

- 纯 HTTP/2 应用（如 gRPC）
- 需要利用 HTTP/2 特性（多路复用、服务器推送等）
- 对性能有极致要求
- 客户端可以保证支持 HTTP/2

## 文件结构

```
galay-http/server/
├── HttpServer.h/.cc        # HTTP/1.x server (无加密)
├── HttpsServer.h/.cc       # HTTPS + HTTP/1.x server
└── Http2Server.h/.cc       # HTTP/2 over TLS (h2) server [新增]
```

## 测试示例

完整的测试示例请参考：
- `test/test_http2_server_h2.cc` - HTTP/2 服务器测试
- `test/html/test_h2.html` - 浏览器测试页面

## 迁移指南

如果你之前使用 `HttpsServer` 处理 HTTP/2，可以这样迁移：

### 之前

```cpp
HttpsServer server = HttpsServerBuilder("cert.pem", "key.pem")
                        .enableHttp2(true)
                        .build();
                        
server.run(runtime, http1Router, http2Callbacks);
```

### 现在

```cpp
Http2Server server = Http2ServerBuilder("cert.pem", "key.pem")
                        .build();
                        
server.run(runtime, http2Callbacks);
```

## 性能优势

1. **减少协议检测开销**：不需要在每个连接上检测协议
2. **优化 ALPN 协商**：只协商 h2，减少选择逻辑
3. **专用代码路径**：HTTP/2 处理路径更短、更快
4. **更少的分支**：减少 if/else 判断，提高 CPU 缓存命中率

## 总结

`Http2Server` 是一个专门为 HTTP/2 over TLS 设计的服务器类，它提供了：

✅ **简洁的 API**：专注于 HTTP/2，不包含 HTTP/1.x 的复杂性  
✅ **自动 ALPN 配置**：自动配置为仅支持 h2  
✅ **回调驱动**：基于帧回调的编程模型，更适合 HTTP/2  
✅ **高性能**：专门优化的 HTTP/2 处理路径  
✅ **易于使用**：清晰的接口和完整的文档  

对于需要构建高性能 HTTP/2 服务的场景，强烈推荐使用 `Http2Server`！

