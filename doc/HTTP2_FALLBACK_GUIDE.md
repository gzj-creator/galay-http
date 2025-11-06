# HTTP/2 Server 降级支持

## 概述

`Http2Server` 现在支持 ALPN 协议协商和优雅降级到 HTTP/1.1。当客户端不支持 HTTP/2 时，服务器可以自动降级到 HTTP/1.1 处理请求。

## 使用场景

### 场景 1：仅支持 HTTP/2（默认）

如果你只想支持 HTTP/2，不需要考虑降级：

```cpp
#include "galay-http/server/Http2Server.h"

Http2Server server = Http2ServerBuilder("cert.pem", "key.pem")
                        .addListen(Host("0.0.0.0", 8443))
                        .build();

Http2Callbacks callbacks;
callbacks.on_headers = onHeaders;
callbacks.on_data = onData;

// 仅支持 h2，不支持 HTTP/2 的客户端会被拒绝
server.run(runtime, callbacks);
server.wait();
```

**ALPN 配置**: `h2` only  
**行为**: 客户端必须支持 HTTP/2，否则连接被拒绝

### 场景 2：支持降级到 HTTP/1.1（使用路由器）

如果你想兼容不支持 HTTP/2 的客户端，可以提供 HTTP/1.1 降级支持：

```cpp
#include "galay-http/server/Http2Server.h"
#include "galay-http/kernel/http/HttpsRouter.h"

// HTTP/2 服务器
Http2Server server = Http2ServerBuilder("cert.pem", "key.pem")
                        .addListen(Host("0.0.0.0", 8443))
                        .build();

// HTTP/2 回调
Http2Callbacks http2Callbacks;
http2Callbacks.on_headers = onHeaders;
http2Callbacks.on_data = onData;

// HTTP/1.1 降级路由
HttpsRouter http1Router;
HttpsRouteMap routes = {
    {"/", handleIndex},
    {"/api/hello", handleHello}
};
http1Router.addRoute<GET>(routes);
http1Router.addRoute<POST>(routes);

// 运行服务器（支持降级）
server.run(runtime, http2Callbacks, http1Router);
server.wait();
```

**ALPN 配置**: `h2, http/1.1` (h2 优先)  
**行为**: 
- 支持 HTTP/2 的客户端 → 使用 HTTP/2
- 不支持 HTTP/2 的客户端 → 降级到 HTTP/1.1

### 场景 3：支持降级到 HTTP/1.1（使用自定义处理器）

如果你想自己处理降级逻辑，可以提供自定义的 HTTP/1.1 处理器：

```cpp
#include "galay-http/server/Http2Server.h"

// HTTP/1.1 降级处理器
Coroutine<nil> handleHttp1Fallback(HttpsConnection& conn)
{
    // 自定义 HTTP/1.1 处理逻辑
    auto reader = conn.getRequestReader({});
    auto writer = conn.getResponseWriter({});
    
    auto request_res = co_await reader.getRequest();
    if (request_res) {
        HttpResponse response;
        response.header().code() = HttpStatusCode::OK_200;
        response.setBodyStr("HTTP/1.1 fallback response");
        co_await writer.reply(response);
    }
    
    co_await conn.close();
    co_return nil();
}

int main()
{
    // 创建服务器
    Http2Server server = Http2ServerBuilder("cert.pem", "key.pem")
                            .addListen(Host("0.0.0.0", 8443))
                            .build();
    
    // HTTP/2 回调
    Http2Callbacks http2Callbacks;
    http2Callbacks.on_headers = onHeaders;
    http2Callbacks.on_data = onData;
    
    // 运行服务器（使用自定义降级处理器）
    server.run(runtime, http2Callbacks, handleHttp1Fallback);
    server.wait();
}
```

**ALPN 配置**: `h2, http/1.1` (h2 优先)  
**行为**:
- 支持 HTTP/2 的客户端 → 使用 HTTP/2
- 不支持 HTTP/2 的客户端 → 调用自定义处理器

## API 参考

### Http2Server::run() 重载

#### 1. 仅 HTTP/2（无降级）

```cpp
void run(Runtime& runtime,
        const Http2Callbacks& callbacks,
        Http2Settings params = Http2Settings());
```

- **ALPN**: h2 only
- **降级**: 不支持
- **适用**: 纯 HTTP/2 应用

#### 2. HTTP/2 + HTTP/1.1 降级（使用路由）

```cpp
void run(Runtime& runtime,
        const Http2Callbacks& http2Callbacks,
        HttpsRouter& http1Router,
        Http2Settings http2Params = Http2Settings(),
        HttpSettings http1Params = HttpSettings());
```

- **ALPN**: h2, http/1.1 (h2 优先)
- **降级**: 自动降级到 HTTP/1.1
- **HTTP/1.1 处理**: 使用路由器
- **适用**: 需要兼容旧客户端

#### 3. HTTP/2 + HTTP/1.1 降级（使用自定义处理器）

```cpp
void run(Runtime& runtime,
        const Http2Callbacks& http2Callbacks,
        const Http1FallbackFunc& http1Fallback,
        Http2Settings http2Params = Http2Settings());
```

- **ALPN**: h2, http/1.1 (h2 优先)
- **降级**: 自动降级到 HTTP/1.1
- **HTTP/1.1 处理**: 使用自定义处理器
- **适用**: 需要自定义降级逻辑

## 完整示例

### 仅 HTTP/2（推荐用于新应用）

```cpp
#include "galay/kernel/runtime/Runtime.h"
#include "galay-http/server/Http2Server.h"
#include <iostream>

using namespace galay;
using namespace galay::http;

// HTTP/2 HEADERS 回调
Coroutine<nil> onHeaders(Http2Connection& conn, 
                         uint32_t stream_id,
                         const std::map<std::string, std::string>& headers,
                         bool end_stream)
{
    if (end_stream) {
        // 发送响应
        auto writer = conn.getWriter({});
        
        HpackEncoder encoder;
        std::vector<HpackHeaderField> response_headers = {
            {":status", "200"},
            {"content-type", "text/plain"},
            {"content-length", "12"}
        };
        std::string encoded_headers = encoder.encodeHeaders(response_headers);
        
        co_await writer.sendHeaders(stream_id, encoded_headers, false, true);
        co_await writer.sendData(stream_id, "Hello HTTP/2", true);
    }
    co_return nil();
}

Coroutine<nil> onData(Http2Connection& conn,
                      uint32_t stream_id,
                      const std::string& data,
                      bool end_stream)
{
    if (end_stream) {
        // 处理数据并发送响应
    }
    co_return nil();
}

int main()
{
    RuntimeBuilder builder;
    auto runtime = builder.build();
    runtime.start();
    
    // 仅支持 HTTP/2
    Http2Server server = Http2ServerBuilder("server.crt", "server.key")
                            .addListen(Host("0.0.0.0", 8443))
                            .build();
    
    Http2Callbacks callbacks;
    callbacks.on_headers = onHeaders;
    callbacks.on_data = onData;
    
    std::cout << "HTTP/2 server started (h2 only)" << std::endl;
    std::cout << "ALPN: h2" << std::endl;
    
    server.run(runtime, callbacks);
    server.wait();
    
    return 0;
}
```

### HTTP/2 + HTTP/1.1 降级（推荐用于兼容性）

```cpp
#include "galay/kernel/runtime/Runtime.h"
#include "galay-http/server/Http2Server.h"
#include "galay-http/kernel/http/HttpsRouter.h"
#include <iostream>

using namespace galay;
using namespace galay::http;

// HTTP/2 回调（同上）
Coroutine<nil> onHeaders(Http2Connection& conn, ...) { ... }
Coroutine<nil> onData(Http2Connection& conn, ...) { ... }

// HTTP/1.1 降级处理器
Coroutine<nil> handleHttp1Index(HttpRequest& request, 
                                HttpsConnection& conn, 
                                HttpsParams params)
{
    auto writer = conn.getResponseWriter({});
    
    HttpResponse response;
    response.header().code() = HttpStatusCode::OK_200;
    response.header().version() = HttpVersion::Http_Version_1_1;
    response.header().headerPairs().addHeaderPair("Content-Type", "text/plain");
    response.setBodyStr("Hello HTTP/1.1 (fallback)");
    
    co_await writer.reply(response);
    co_await conn.close();
    
    co_return nil();
}

int main()
{
    RuntimeBuilder builder;
    auto runtime = builder.build();
    runtime.start();
    
    // 支持 HTTP/2 + HTTP/1.1 降级
    Http2Server server = Http2ServerBuilder("server.crt", "server.key")
                            .addListen(Host("0.0.0.0", 8443))
                            .build();
    
    // HTTP/2 回调
    Http2Callbacks http2Callbacks;
    http2Callbacks.on_headers = onHeaders;
    http2Callbacks.on_data = onData;
    
    // HTTP/1.1 降级路由
    HttpsRouter http1Router;
    HttpsRouteMap routes = {{"/", handleHttp1Index}};
    http1Router.addRoute<GET>(routes);
    
    std::cout << "HTTP/2 server started (with HTTP/1.1 fallback)" << std::endl;
    std::cout << "ALPN: h2, http/1.1" << std::endl;
    
    server.run(runtime, http2Callbacks, http1Router);
    server.wait();
    
    return 0;
}
```

## 测试降级功能

### 测试 HTTP/2 客户端

```bash
# curl 支持 HTTP/2
curl -v --http2 https://localhost:8443/ --insecure

# 输出应该显示: ALPN, server accepted to use h2
```

### 测试 HTTP/1.1 降级

```bash
# 强制使用 HTTP/1.1
curl -v --http1.1 https://localhost:8443/ --insecure

# 输出应该显示: ALPN, server accepted to use http/1.1
```

### 使用浏览器测试

现代浏览器默认支持 HTTP/2：

```bash
# Chrome/Firefox 会优先使用 HTTP/2
https://localhost:8443/

# 在开发者工具的 Network 面板中查看协议列
# Protocol 列应该显示: h2
```

## ALPN 协商流程

### 仅 HTTP/2 模式

```
Client                 Server
  |                      |
  |------- TLS ClientHello ------>|
  |        ALPN: h2                |
  |                      |
  |<------ TLS ServerHello -------|
  |        ALPN: h2                |
  |                      |
  |====== HTTP/2 Connection =======|
```

### 降级模式

```
Client (HTTP/2)       Server
  |                      |
  |------- TLS ClientHello ------>|
  |        ALPN: h2, http/1.1      |
  |                      |
  |<------ TLS ServerHello -------|
  |        ALPN: h2                |  <-- 选择 h2
  |                      |
  |====== HTTP/2 Connection =======|


Client (HTTP/1.1)     Server
  |                      |
  |------- TLS ClientHello ------>|
  |        ALPN: http/1.1          |
  |                      |
  |<------ TLS ServerHello -------|
  |        ALPN: http/1.1          |  <-- 降级到 http/1.1
  |                      |
  |====== HTTP/1.1 Connection =====|
```

## 性能考虑

### 仅 HTTP/2 模式

- ✅ **无协议检测开销**：不需要运行时检测
- ✅ **更快的连接建立**：ALPN 直接选择 h2
- ✅ **专用代码路径**：只执行 HTTP/2 处理逻辑

### 降级模式

- ⚠️ **需要协议检测**：每个连接都要检测 ALPN 结果
- ⚠️ **两套代码路径**：HTTP/2 和 HTTP/1.1 都要加载
- ✅ **兼容性更好**：支持旧客户端

### 建议

- **新应用**：使用仅 HTTP/2 模式，获得最佳性能
- **需要兼容**：使用降级模式，支持旧客户端
- **API 服务**：考虑使用版本化的端口（8443 for HTTP/2, 8444 for HTTP/1.1）

## 总结

`Http2Server` 的降级支持提供了：

✅ **灵活的 API**：通过不同的 `run()` 方法选择是否降级  
✅ **自动 ALPN 配置**：根据运行模式自动配置协议列表  
✅ **两种降级方式**：路由器或自定义处理器  
✅ **零配置**：不需要手动配置 ALPN  
✅ **高性能**：仅 HTTP/2 模式没有额外开销  

选择适合你的场景的运行模式，享受 HTTP/2 的高性能和灵活的降级支持！

