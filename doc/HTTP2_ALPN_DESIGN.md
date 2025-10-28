# HTTP/2 协议协商机制

## 问题分析

### 当前实现的问题 ⚠️

```cpp
// HttpWriter::upgradeToHttp2()  - 使用 HTTP/1.1 Upgrade ✅ 正确
// HttpsWriter::upgradeToHttp2() - 使用 HTTP/1.1 Upgrade ❌ 不够好
```

**当前两者的实现几乎相同，都使用 HTTP/1.1 Upgrade 机制。但这不是最优的！**

## 正确的协商机制

### 1. h2c (HTTP/2 over TCP) - HTTP/1.1 Upgrade

```
客户端请求：
GET /index.html HTTP/1.1
Host: server.example.com
Connection: Upgrade, HTTP2-Settings
Upgrade: h2c
HTTP2-Settings: <base64url encoded SETTINGS payload>

服务器响应：
HTTP/1.1 101 Switching Protocols
Connection: Upgrade
Upgrade: h2c

[切换到 HTTP/2]
```

✅ **当前实现**: `HttpWriter::upgradeToHttp2()` - 正确

### 2. h2 (HTTP/2 over TLS) - ALPN (推荐)

**ALPN 在 TLS 握手时完成协商，不需要 HTTP/1.1 Upgrade！**

```
客户端 TLS ClientHello：
    ALPN extension: [h2, http/1.1]
    
服务器 TLS ServerHello：
    ALPN selected: h2

[TLS 握手完成后直接使用 HTTP/2]
```

#### ALPN 的优势

1. **更高效**: 
   - 无需 HTTP/1.1 请求
   - 无需 101 Switching Protocols 响应
   - 减少一次往返（RTT）

2. **标准做法**:
   - RFC 7540 推荐
   - 浏览器默认方式
   - 所有主流 HTTPS 服务器的实现

3. **性能**:
   ```
   HTTP/1.1 Upgrade: 3 RTT (TLS握手 + HTTP请求 + 101响应)
   ALPN:            2 RTT (TLS握手包含协商)
   ```

## OpenSSL ALPN 实现

### 服务器端（C++）

```cpp
#include <openssl/ssl.h>

// ALPN 回调函数
int alpn_select_cb(SSL *ssl,
                  const unsigned char **out,
                  unsigned char *outlen,
                  const unsigned char *in,
                  unsigned int inlen,
                  void *arg)
{
    // 服务器支持的协议列表（优先级从高到低）
    static const unsigned char protos[] = {
        2, 'h', '2',           // HTTP/2
        8, 'h', 't', 't', 'p', '/', '1', '.', '1'  // HTTP/1.1
    };
    
    // 选择协议
    if (SSL_select_next_proto((unsigned char **)out, outlen,
                             protos, sizeof(protos),
                             in, inlen) == OPENSSL_NPN_NEGOTIATED)
    {
        return SSL_TLSEXT_ERR_OK;
    }
    
    return SSL_TLSEXT_ERR_NOACK;
}

// 设置 ALPN
void setup_alpn(SSL_CTX *ctx)
{
    SSL_CTX_set_alpn_select_cb(ctx, alpn_select_cb, nullptr);
}

// 检查协商结果
void check_alpn(SSL *ssl)
{
    const unsigned char *alpn = nullptr;
    unsigned int alpnlen = 0;
    
    SSL_get0_alpn_selected(ssl, &alpn, &alpnlen);
    
    if (alpnlen == 2 && memcmp(alpn, "h2", 2) == 0) {
        // 使用 HTTP/2
        std::cout << "Negotiated: HTTP/2" << std::endl;
    } else if (alpnlen == 8 && memcmp(alpn, "http/1.1", 8) == 0) {
        // 使用 HTTP/1.1
        std::cout << "Negotiated: HTTP/1.1" << std::endl;
    }
}
```

### 客户端（浏览器）

浏览器自动在 TLS ClientHello 中发送 ALPN：

```
Extension: application_layer_protocol_negotiation
    ALPN Extension Length: 14
    ALPN Protocol
        ALPN string length: 2
        ALPN Next Protocol: h2
        ALPN string length: 8
        ALPN Next Protocol: http/1.1
```

## 实现方案

### 方案 1: 在 Galay 框架层实现（推荐）

ALPN 是 TLS 层的功能，应该在 `galay` 框架的 `AsyncSslSocket` 中实现：

```cpp
// galay/kernel/async/Socket.h
class AsyncSslSocket {
public:
    // 设置 ALPN 协议列表
    void setAlpnProtocols(const std::vector<std::string>& protocols);
    
    // 获取协商的协议
    std::string getNegotiatedProtocol() const;
    
private:
    SSL* m_ssl;
};

// 实现
void AsyncSslSocket::setAlpnProtocols(const std::vector<std::string>& protocols)
{
    // 构建 ALPN 协议列表
    std::vector<unsigned char> alpn_protos;
    for (const auto& proto : protocols) {
        alpn_protos.push_back(proto.size());
        alpn_protos.insert(alpn_protos.end(), proto.begin(), proto.end());
    }
    
    SSL_set_alpn_protos(m_ssl, alpn_protos.data(), alpn_protos.size());
}

std::string AsyncSslSocket::getNegotiatedProtocol() const
{
    const unsigned char *alpn = nullptr;
    unsigned int alpnlen = 0;
    SSL_get0_alpn_selected(m_ssl, &alpn, &alpnlen);
    
    if (alpn && alpnlen > 0) {
        return std::string(reinterpret_cast<const char*>(alpn), alpnlen);
    }
    return "http/1.1";  // 默认
}
```

### 方案 2: 在 HttpsServer 中配置

```cpp
class HttpsServer {
public:
    // 启用 ALPN 并设置支持的协议
    void enableAlpn(const std::vector<std::string>& protocols = {"h2", "http/1.1"});
    
private:
    Coroutine<nil> handleConnection(Runtime& runtime, HttpRouter& router, 
                                   HttpSettings params, AsyncSslSocket socket)
    {
        // TLS 握手已完成，检查协商的协议
        std::string protocol = socket.getNegotiatedProtocol();
        
        if (protocol == "h2") {
            // 直接使用 HTTP/2，无需 Upgrade
            Http2Connection http2Conn = Http2Connection::from(socket);
            // 处理 HTTP/2...
        } else {
            // 使用 HTTP/1.1
            HttpsConnection conn(std::move(socket), ...);
            // 处理 HTTP/1.1...
        }
        
        co_return nil();
    }
};
```

### 方案 3: 自动检测（最优）

```cpp
Coroutine<nil> HttpsServer::handleConnection(...)
{
    AsyncFactory factory = runtime.getAsyncFactory();
    
    // 检查 ALPN 协商结果
    std::string protocol = socket.getNegotiatedProtocol();
    
    if (protocol == "h2") {
        HTTP_LOG_INFO("[HttpsServer] Using HTTP/2 (via ALPN)");
        
        // 直接创建 HTTP/2 连接，无需 Upgrade
        Http2Connection http2Conn(std::move(socket), factory.getTimerGenerator());
        
        // 处理 HTTP/2 通信
        // ...
        
    } else {
        HTTP_LOG_INFO("[HttpsServer] Using HTTP/1.1 (fallback)");
        
        // 使用 HTTP/1.1
        HttpsConnection conn(std::move(socket), factory.getTimerGenerator());
        
        // 标准 HTTP/1.1 处理（支持 Upgrade 到 h2）
        // ...
    }
    
    co_return nil();
}
```

## 对比总结

### h2c (HTTP)

```cpp
// 实现方式：HTTP/1.1 Upgrade
HttpWriter::upgradeToHttp2()

// 流程：
1. 客户端发送 HTTP/1.1 请求 + Upgrade: h2c
2. 服务器返回 101 Switching Protocols
3. 切换到 HTTP/2

// 使用场景：
- 内部服务通信
- curl 测试
- 不需要加密的场景
```

### h2 (HTTPS) - 当前实现 ⚠️

```cpp
// 当前：仍使用 HTTP/1.1 Upgrade（不够好）
HttpsWriter::upgradeToHttp2()

// 流程：
1. TLS 握手（无 ALPN）
2. 客户端发送 HTTP/1.1 请求 + Upgrade: h2
3. 服务器返回 101 Switching Protocols  
4. 切换到 HTTP/2

// 问题：
- 多一次 RTT
- 不是浏览器的标准方式
```

### h2 (HTTPS) - 正确实现 ✅

```cpp
// 应该：使用 ALPN
在 TLS 握手时协商

// 流程：
1. TLS 握手（包含 ALPN 协商）
   ClientHello: ALPN [h2, http/1.1]
   ServerHello: ALPN selected: h2
2. 握手完成后直接使用 HTTP/2

// 优势：
- 减少一次 RTT
- 浏览器标准方式
- 更高效
```

## 实现优先级

1. **短期**: 保持当前实现（HTTP/1.1 Upgrade）
   - 功能正确
   - curl 可用
   - 浏览器可以降级使用

2. **中期**: 添加 ALPN 支持到 galay 框架
   - 在 `AsyncSslSocket` 中添加 ALPN API
   - `HttpsServer` 检测协商结果
   - 自动选择 HTTP/2 或 HTTP/1.1

3. **长期**: 完全移除 HTTPS 的 Upgrade 方式
   - HTTPS 只使用 ALPN
   - HTTP 继续使用 Upgrade（h2c）

## 测试验证

### 使用 OpenSSL 命令行测试 ALPN

```bash
# 测试服务器的 ALPN 支持
openssl s_client -connect localhost:8443 -alpn h2 -servername localhost

# 查看协商结果
# Protocol  : TLSv1.3
# ALPN protocol: h2
```

### 使用 curl 测试

```bash
# curl 使用 ALPN（如果服务器支持）
curl -v --http2 https://localhost:8443/

# 输出会显示：
# * ALPN, offering h2
# * ALPN, server accepted to use h2
```

### 浏览器开发者工具

```
Network 标签：
Name        Protocol    
/index      h2          ← ALPN 协商的 HTTP/2
/api/data   h2
```

## 参考资料

- RFC 7540: HTTP/2 (Section 3.3 - Starting HTTP/2 for "https" URIs)
- RFC 7301: ALPN (Application-Layer Protocol Negotiation)
- OpenSSL ALPN API: `SSL_CTX_set_alpn_select_cb()`

## 结论

- **h2c**: 必须使用 HTTP/1.1 Upgrade ✅ 当前实现正确
- **h2**: 应该使用 ALPN ⚠️ 需要在 galay 框架层实现
- **当前**: 两者都用 Upgrade，功能正确但不是最优

**建议**: 向 galay 框架提交 PR，添加 ALPN 支持！

