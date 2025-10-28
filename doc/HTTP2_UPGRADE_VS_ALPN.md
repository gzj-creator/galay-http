# HTTP/2 Upgrade vs ALPN

## 核心问题

**Q: 通过 ALPN 协商 HTTP/2 后，还需要 `upgradeToHttp2()` 吗？**

**A: 不需要！ALPN 和 HTTP/1.1 Upgrade 是两种不同的 HTTP/2 建立方式。**

## 两种 HTTP/2 建立方式

### 方式 1: ALPN（h2）- HTTPS 场景

**适用于**: HTTPS 连接（TLS）

**流程**:
```
1. Client → Server: TLS ClientHello
   {
       ...
       ALPN extension: ["h2", "http/1.1"]
   }

2. Server → Client: TLS ServerHello
   {
       ...
       ALPN extension: "h2"  ← 服务器选择了 h2
   }

3. TLS 握手完成
   ✅ 连接已经是 HTTP/2！
   ✅ 直接发送 HTTP/2 帧（SETTINGS, HEADERS, DATA）
   ❌ 不需要 HTTP/1.1 Upgrade
```

**代码示例**:
```cpp
// 1. 配置 ALPN
SSL_CTX* ctx = galay::getGlobalSSLCtx();
configureServerAlpn(ctx, AlpnProtocolList::http2WithFallback());

// 2. 在连接建立后检测协议
std::string protocol = getAlpnProtocol(ssl);

if (protocol == "h2") {
    // 3. 直接创建 Http2Connection
    Http2Connection http2Conn = Http2Connection::from(httpsConn);
    
    // 4. 发送/接收 HTTP/2 帧
    co_await http2Conn.sendSettings(...);
    
    // ❌ 不需要调用 upgradeToHttp2()
}
```

### 方式 2: HTTP/1.1 Upgrade（h2c）- HTTP 场景

**适用于**: HTTP 连接（cleartext，非加密）

**流程**:
```
1. Client → Server: HTTP/1.1 请求
   GET / HTTP/1.1
   Host: example.com
   Connection: Upgrade, HTTP2-Settings
   Upgrade: h2c
   HTTP2-Settings: <base64url 编码的 SETTINGS>

2. Server → Client: 101 响应
   HTTP/1.1 101 Switching Protocols
   Connection: Upgrade
   Upgrade: h2c

3. 协议切换完成
   ✅ 从此开始使用 HTTP/2
   ✅ 发送 HTTP/2 SETTINGS 帧
```

**代码示例**:
```cpp
// HTTP/1.1 连接处理
HttpConnection conn = ...;
HttpWriter writer = conn.getResponseWriter({});

// 检测是否是 Upgrade 请求
if (request.header().hasUpgrade() && 
    request.header().getUpgrade() == "h2c") {
    
    // 执行 HTTP/1.1 Upgrade
    auto result = co_await writer.upgradeToHttp2(request);
    
    if (result) {
        // Upgrade 成功，切换到 HTTP/2
        Http2Connection http2Conn = Http2Connection::from(conn);
        // ... 处理 HTTP/2 ...
    }
}
```

## 对比表格

| 特性 | ALPN (h2) | HTTP/1.1 Upgrade (h2c) |
|------|-----------|------------------------|
| **传输层** | TLS（加密） | TCP（明文） |
| **协议标识** | h2 | h2c |
| **协商时机** | TLS 握手期间 | HTTP 请求/响应 |
| **往返次数** | 2 RTT（TLS 握手） | 3 RTT（TCP + HTTP Upgrade） |
| **是否需要 Upgrade** | ❌ 不需要 | ✅ 需要 |
| **浏览器支持** | ✅ 所有现代浏览器 | ⚠️ 浏览器不支持 h2c |
| **适用场景** | 生产环境 HTTPS | 内部服务、测试 |
| **实现类** | `HttpsWriter` | `HttpWriter` |

## 设计原则

### HttpWriter（HTTP 连接）
```cpp
class HttpWriter 
{
public:
    // ✅ 应该有这个方法
    // 用于 h2c (HTTP/1.1 Upgrade)
    AsyncResult<std::expected<void, HttpError>> 
        upgradeToHttp2(HttpRequest& request, ...);
};
```

### HttpsWriter（HTTPS 连接）
```cpp
class HttpsWriter 
{
public:
    // ❌ 不应该有这个方法
    // HTTPS 应该使用 ALPN，不需要 Upgrade
    
    // 如果实现了，只是为了兼容性
    // 但实际使用中应该避免调用
};
```

## 实际使用建议

### HTTPS 场景（推荐）

```cpp
// ✅ 正确的方式：使用 ALPN

// 1. 初始化并配置 ALPN
galay::initializeSSLServerEnv("cert.pem", "key.pem");
SSL_CTX* ctx = galay::getGlobalSSLCtx();
configureServerAlpn(ctx, AlpnProtocolList::http2WithFallback());

// 2. 在连接处理中
HttpsConnection conn = ...;

// 方案 A: 在 TLS 层检测（需要访问 SSL*）
// std::string protocol = getAlpnProtocol(ssl);
// if (protocol == "h2") { ... }

// 方案 B: 尝试读取第一个请求
auto reader = conn.getRequestReader({});
auto request_res = co_await reader.getRequest();

if (!request_res) {
    // 可能是 HTTP/2 连接前言
    // 直接创建 Http2Connection
    Http2Connection http2Conn = Http2Connection::from(conn);
    // 处理 HTTP/2...
} else {
    // HTTP/1.1 请求
    // 正常处理...
}
```

### HTTP 场景

```cpp
// ✅ 使用 HTTP/1.1 Upgrade

HttpConnection conn = ...;
HttpWriter writer = conn.getResponseWriter({});

// 检测 Upgrade 请求
if (request.header().getHeader("Upgrade") == "h2c") {
    auto result = co_await writer.upgradeToHttp2(request);
    
    if (result) {
        // 切换到 HTTP/2
        Http2Connection http2Conn = Http2Connection::from(conn);
        // ...
    }
}
```

## 常见错误

### ❌ 错误 1: HTTPS 使用 Upgrade

```cpp
// ❌ 错误：HTTPS 连接不需要 Upgrade
HttpsConnection conn = ...;
HttpsWriter writer = conn.getResponseWriter({});
co_await writer.upgradeToHttp2(request);  // 不需要！
```

**正确做法**: 使用 ALPN 配置，连接建立时就已经是 HTTP/2

### ❌ 错误 2: HTTP 不检测 Upgrade 头

```cpp
// ❌ 错误：直接假设是 HTTP/2
HttpConnection conn = ...;
Http2Connection http2Conn = Http2Connection::from(conn);  // 错误！
```

**正确做法**: 先检测 Upgrade 头，执行 Upgrade 流程

### ❌ 错误 3: 混淆 h2 和 h2c

```cpp
// ❌ 错误：ALPN 协商 h2c
configureServerAlpn(ctx, "h2c");  // h2c 不能用 ALPN！
```

**正确做法**: 
- ALPN 使用 `h2`
- HTTP/1.1 Upgrade 使用 `h2c`

## 实现建议

### 1. 移除 HttpsWriter::upgradeToHttp2()

```cpp
// 当前（不推荐）
class HttpsWriter {
    AsyncResult<...> upgradeToHttp2(...);  // ❌ 应该移除
};

// 建议
class HttpsWriter {
    // ❌ 移除 upgradeToHttp2()
    // HTTPS 应该只使用 ALPN
};
```

### 2. HttpsServer 自动处理 ALPN

```cpp
class HttpsServer {
    Coroutine<nil> handleConnection(..., AsyncSslSocket socket) {
        // 1. 检测 ALPN 协商结果
        std::string protocol = getAlpnProtocol(socket.getSSL());
        
        if (protocol == "h2") {
            // 2. 直接使用 HTTP/2
            HttpsConnection conn(std::move(socket), ...);
            Http2Connection http2Conn = Http2Connection::from(conn);
            co_await handleHttp2Connection(http2Conn);
        } else {
            // 3. 使用 HTTP/1.1
            HttpsConnection conn(std::move(socket), ...);
            co_await handleHttp1Connection(conn);
        }
    }
};
```

### 3. 提供辅助方法

```cpp
// 在 HttpsConnection 中添加
class HttpsConnection {
public:
    // 获取协商的 ALPN 协议
    std::string getAlpnProtocol() const {
        return galay::http::getAlpnProtocol(m_socket.getSSL());
    }
    
    // 检测是否协商了 HTTP/2
    bool isHttp2() const {
        return getAlpnProtocol() == "h2";
    }
};
```

## 总结

| 场景 | 协议 | 方法 | 类 |
|------|------|------|-----|
| **HTTPS** | h2 | ALPN | `HttpsConnection` |
| **HTTP** | h2c | HTTP/1.1 Upgrade | `HttpConnection` + `HttpWriter::upgradeToHttp2()` |

**关键点**:
1. ✅ ALPN 用于 HTTPS (h2)，不需要 `upgradeToHttp2()`
2. ✅ HTTP/1.1 Upgrade 用于 HTTP (h2c)，需要 `upgradeToHttp2()`
3. ❌ 不要在 HTTPS 连接上调用 `upgradeToHttp2()`
4. ✅ 浏览器标准是 ALPN (h2)，生产环境推荐使用

## 参考资料

- [RFC 7540 - HTTP/2](https://tools.ietf.org/html/rfc7540)
  - Section 3.2: Starting HTTP/2 for "http" URIs (h2c)
  - Section 3.3: Starting HTTP/2 for "https" URIs (h2)
- [RFC 7301 - ALPN](https://tools.ietf.org/html/rfc7301)
- [IANA ALPN Protocol IDs](https://www.iana.org/assignments/tls-extensiontype-values/tls-extensiontype-values.xhtml#alpn-protocol-ids)

