# HttpsServer 简化说明

## 重要变更

**HttpsServer 已简化为仅支持 HTTPS + HTTP/1.x**

如果你需要 HTTP/2 over TLS (h2) 支持，请使用独立的 `Http2Server` 类。

## 变更内容

### 移除的功能

以下功能已从 `HttpsServer` 中移除：

1. **HTTP/2 自动检测** - 不再自动检测和处理 HTTP/2 连接
2. **ALPN 配置** - 不再配置 ALPN 协议列表
3. **enableHttp2() 方法** - 已移除
4. **HTTP/2 回调支持** - 不再支持 `Http2Callbacks`
5. **混合路由** - 不再需要同时提供 HTTP/1.1 和 HTTP/2 处理器

### 保留的功能

`HttpsServer` 现在专注于：

✅ HTTPS + HTTP/1.x 连接  
✅ TLS 加密  
✅ 路由和处理器  
✅ 简洁的 API  

## 迁移指南

### 场景 1：仅使用 HTTP/1.x (不需要改动)

如果你之前只使用 HTTP/1.x，不需要任何改动：

```cpp
// 之前 ✅
HttpsServer server = HttpsServerBuilder("cert.pem", "key.pem")
                        .addListen(Host("0.0.0.0", 8443))
                        .build();

HttpsRouter router;
// ... 配置路由 ...

server.run(runtime, router);
server.wait();

// 现在 ✅ (完全相同)
HttpsServer server = HttpsServerBuilder("cert.pem", "key.pem")
                        .addListen(Host("0.0.0.0", 8443))
                        .build();

HttpsRouter router;
// ... 配置路由 ...

server.run(runtime, router);
server.wait();
```

### 场景 2：使用 HTTP/2 (需要迁移)

如果你之前使用 `HttpsServer` 处理 HTTP/2，需要迁移到 `Http2Server`：

#### 之前（已废弃）

```cpp
// ❌ 旧方式：HttpsServer 混合处理
HttpsServer server = HttpsServerBuilder("cert.pem", "key.pem")
                        .enableHttp2(true)
                        .build();

HttpsRouter http1Router;
// ... 配置 HTTP/1.1 路由 ...

Http2Callbacks http2Callbacks;
http2Callbacks.on_headers = onHeaders;
http2Callbacks.on_data = onData;

server.run(runtime, http1Router, http2Callbacks);
```

#### 现在（推荐）

```cpp
// ✅ 新方式：使用独立的 Http2Server
#include "galay-http/server/Http2Server.h"

Http2Server server = Http2ServerBuilder("cert.pem", "key.pem")
                        .addListen(Host("0.0.0.0", 8443))
                        .build();

Http2Callbacks http2Callbacks;
http2Callbacks.on_headers = onHeaders;
http2Callbacks.on_data = onData;

server.run(runtime, http2Callbacks);
server.wait();
```

### 场景 3：同时需要 HTTP/1.x 和 HTTP/2

如果你需要同时支持两种协议，请运行两个独立的服务器：

```cpp
// HTTP/1.x 服务器
HttpsServer http1Server = HttpsServerBuilder("cert.pem", "key.pem")
                            .addListen(Host("0.0.0.0", 8443))
                            .build();

HttpsRouter router;
// ... 配置路由 ...

// HTTP/2 服务器
Http2Server http2Server = Http2ServerBuilder("cert.pem", "key.pem")
                            .addListen(Host("0.0.0.0", 8444))  // 不同端口
                            .build();

Http2Callbacks callbacks;
// ... 配置回调 ...

// 在不同线程或端口运行
std::thread t1([&]() {
    http1Server.run(runtime1, router);
    http1Server.wait();
});

std::thread t2([&]() {
    http2Server.run(runtime2, callbacks);
    http2Server.wait();
});

t1.join();
t2.join();
```

## API 对比

### HttpsServer (现在)

```cpp
class HttpsServer {
public:
    // 自定义处理器
    void run(Runtime& runtime, const HttpsConnFunc& handler);
    
    // 使用路由（推荐）
    void run(Runtime& runtime, HttpsRouter& router, HttpSettings params = {});
    
    void stop();
    void wait();
};
```

### Http2Server (新增)

```cpp
class Http2Server {
public:
    // 使用回调处理帧（推荐）
    void run(Runtime& runtime, 
            const Http2Callbacks& callbacks,
            Http2Settings params = {});
    
    // 自定义处理器
    void run(Runtime& runtime, const Http2ConnFunc& handler);
    
    void stop();
    void wait();
};
```

## 常见问题

### Q: 为什么要分离 HttpsServer 和 Http2Server？

**A:** 职责分离原则：
- **简化代码**：每个类专注于一种协议
- **提高性能**：减少运行时协议检测开销
- **易于维护**：HTTP/2 的改动不会影响 HTTP/1.x
- **更清晰的 API**：用户明确知道使用哪个服务器

### Q: HttpsServer 还会支持 HTTP/2 吗？

**A:** 不会。`HttpsServer` 现在只专注于 HTTPS + HTTP/1.x。如果需要 HTTP/2，请使用 `Http2Server`。

### Q: 我的项目使用了 enableHttp2()，怎么办？

**A:** 移除 `.enableHttp2()` 调用即可。如果确实需要 HTTP/2，请迁移到 `Http2Server`。

### Q: 迁移会破坏现有代码吗？

**A:** 
- 如果只使用 HTTP/1.x：不会，只需移除 `.enableHttp2()` 调用
- 如果使用 HTTP/2：需要迁移到 `Http2Server`（很简单，见上面的示例）

## 性能优势

新架构的性能优势：

1. **减少分支**：不需要在每个连接上检测协议
2. **更快的连接建立**：没有 ALPN 协商开销（HttpsServer）
3. **专用优化**：可以针对不同协议做专门优化
4. **更少的代码路径**：执行路径更短更快

## 总结

| 特性 | HttpsServer | Http2Server |
|------|-------------|-------------|
| 协议 | HTTPS + HTTP/1.x | HTTP/2 over TLS (h2) |
| ALPN | 不使用 | 自动配置为 h2 only |
| 编程模型 | Router + Handler | Callbacks |
| 适用场景 | 传统 HTTPS 服务 | 高性能 HTTP/2 服务 |

## 更多信息

- [Http2Server 设计文档](HTTP2_SERVER_DESIGN.md)
- [HTTP/2 测试示例](../test/test_http2_server_h2.cc)
- [HTTPS 测试示例](../test/test_https_server.cc)

