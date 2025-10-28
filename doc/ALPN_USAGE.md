# ALPN (Application-Layer Protocol Negotiation) 使用指南

## 概述

ALPN 是 TLS 扩展，允许客户端和服务器在 TLS 握手期间协商应用层协议。这是 HTTP/2 over TLS (h2) 的标准方式。

## 协议支持

### 枚举定义

```cpp
enum class AlpnProtocol
{
    HTTP_2,      // h2 - HTTP/2 over TLS
    HTTP_1_1,    // http/1.1 - HTTP/1.1
    HTTP_1_0,    // http/1.0 - HTTP/1.0 (rarely used)
};
```

### 协议名称映射

| 枚举值 | ALPN 协议名 | 说明 |
|--------|-------------|------|
| `AlpnProtocol::HTTP_2` | `h2` | HTTP/2 over TLS (RFC 7540) |
| `AlpnProtocol::HTTP_1_1` | `http/1.1` | HTTP/1.1 |
| `AlpnProtocol::HTTP_1_0` | `http/1.0` | HTTP/1.0（较少使用） |

**注意**：h2c (HTTP/2 over cleartext) **不使用** ALPN，它使用 HTTP/1.1 Upgrade 机制。

## 预定义配置

### 1. HTTP/2 优先，fallback 到 HTTP/1.1（推荐）

```cpp
auto config = AlpnProtocolList::http2WithFallback();
// 协议优先级：h2 > http/1.1
```

**适用场景**：
- 生产环境的 HTTPS 服务器
- 希望支持现代客户端的 HTTP/2，同时兼容旧客户端

**客户端行为**：
- 支持 HTTP/2 的客户端（现代浏览器、curl）→ 使用 h2
- 不支持 HTTP/2 的客户端 → 使用 http/1.1

### 2. 仅 HTTP/2

```cpp
auto config = AlpnProtocolList::http2Only();
// 协议：h2
```

**适用场景**：
- 内部微服务通信（确保都支持 HTTP/2）
- API 服务器（要求客户端必须支持 HTTP/2）

**注意**：不支持 HTTP/2 的客户端将无法连接。

### 3. 仅 HTTP/1.1

```cpp
auto config = AlpnProtocolList::http11Only();
// 协议：http/1.1
```

**适用场景**：
- 兼容性测试
- 需要强制使用 HTTP/1.1 的场景

### 4. HTTP/1.1 优先，支持 HTTP/2

```cpp
auto config = AlpnProtocolList::http11WithHttp2();
// 协议优先级：http/1.1 > h2
```

**适用场景**：
- 逐步迁移到 HTTP/2
- 需要优先使用 HTTP/1.1 的特殊场景

## 自定义配置

### 基本用法

```cpp
// 自定义协议列表（按优先级排序）
auto custom = AlpnProtocolList({
    AlpnProtocol::HTTP_2,     // 第一优先级
    AlpnProtocol::HTTP_1_1,   // 第二优先级
    AlpnProtocol::HTTP_1_0    // 第三优先级（fallback）
});
```

### 检查配置

```cpp
// 检查是否包含某个协议
if (config.contains(AlpnProtocol::HTTP_2)) {
    // 支持 HTTP/2
}

// 获取默认协议（优先级最高）
AlpnProtocol default_proto = config.defaultProtocol();

// 获取所有协议
for (const auto& protocol : config.protocols()) {
    std::string name = AlpnProtocolRegistry::toString(protocol);
    std::cout << "Support: " << name << std::endl;
}
```

## 服务器端配置

### 完整示例

```cpp
#include <galay/common/Common.h>
#include "galay-http/protoc/alpn/AlpnProtocol.h"
#include "galay-http/server/HttpsServer.h"

using namespace galay;
using namespace galay::http;

int main()
{
    // 1. 初始化 SSL 环境
    if (!galay::initializeSSLServerEnv("server.crt", "server.key")) {
        std::cerr << "Failed to initialize SSL" << std::endl;
        return 1;
    }
    
    // 2. 获取全局 SSL 上下文
    SSL_CTX* ctx = galay::getGlobalSSLCtx();
    
    // 3. 配置 ALPN（选择一种方式）
    
    // 方式 1：使用预定义配置（推荐）
    if (!configureServerAlpn(ctx, AlpnProtocolList::http2WithFallback())) {
        std::cerr << "Failed to configure ALPN" << std::endl;
        return 1;
    }
    
    // 方式 2：使用默认配置（等同于方式 1）
    // if (!configureServerAlpn(ctx)) {
    //     return 1;
    // }
    
    // 方式 3：使用自定义配置
    // auto custom_config = AlpnProtocolList({
    //     AlpnProtocol::HTTP_2,
    //     AlpnProtocol::HTTP_1_1
    // });
    // if (!configureServerAlpn(ctx, custom_config)) {
    //     return 1;
    // }
    
    // 4. 创建 HTTPS 服务器
    RuntimeBuilder runtimebuilder;
    auto runtime = runtimebuilder.build();
    runtime.start();
    
    HttpsServerBuilder builder("server.crt", "server.key");
    HttpsServer server = builder.build();
    server.listen(Host("0.0.0.0", 8443));
    
    // 5. 运行服务器
    HttpRouter router;
    // ... 配置路由 ...
    
    server.run(runtime, router);
    server.wait();
    
    // 6. 清理
    galay::destroySSLEnv();
    return 0;
}
```

### 配置说明

**推荐配置**：
```cpp
configureServerAlpn(ctx, AlpnProtocolList::http2WithFallback());
```

这个配置：
- ✅ 优先使用 HTTP/2（性能更好）
- ✅ 自动 fallback 到 HTTP/1.1（兼容性好）
- ✅ 适用于大多数生产环境

## 客户端配置

```cpp
#include <galay/common/Common.h>
#include "galay-http/protoc/alpn/AlpnProtocol.h"

// 初始化客户端 SSL 环境
galay::initialiszeSSLClientEnv();

// 获取 SSL 上下文
SSL_CTX* ctx = galay::getGlobalSSLCtx();

// 配置客户端 ALPN
if (!configureClientAlpn(ctx, AlpnProtocolList::http2WithFallback())) {
    // 错误处理
}

// 客户端会在 TLS 握手时告诉服务器支持的协议
// 服务器从中选择一个
```

## 协议检测

### 在连接建立后检测协商的协议

```cpp
// 假设你有一个 SSL* 对象
std::string negotiated = getAlpnProtocol(ssl);

if (negotiated == "h2") {
    // 使用 HTTP/2
    std::cout << "Using HTTP/2" << std::endl;
} else if (negotiated == "http/1.1") {
    // 使用 HTTP/1.1
    std::cout << "Using HTTP/1.1" << std::endl;
} else {
    // 没有协商 ALPN（可能是不支持 ALPN 的客户端）
    std::cout << "No ALPN negotiated, using default" << std::endl;
}
```

## 测试 ALPN

### 使用 curl

```bash
# 测试 HTTP/2（会使用 ALPN）
curl -v --http2 https://localhost:8443/api --insecure

# 测试 HTTP/1.1
curl -v --http1.1 https://localhost:8443/api --insecure

# 查看输出中的 ALPN 信息：
# * ALPN, offering h2
# * ALPN, offering http/1.1
# ...
# * ALPN, server accepted to use h2
```

### 使用 OpenSSL 命令行

```bash
# 测试 ALPN 协商
echo "Q" | openssl s_client -connect localhost:8443 -alpn h2

# 查看输出：
# ALPN protocol: h2
```

### 使用浏览器

1. 打开 Chrome DevTools (F12)
2. 访问 `https://localhost:8443/`
3. 在 Network 标签中查看请求
4. Protocol 列应该显示 `h2` 或 `http/1.1`

## 工作原理

### ALPN 握手流程

```
1. Client → Server: 
   TLS ClientHello {
       ...
       ALPN extension: ["h2", "http/1.1"]
   }

2. Server 处理：
   - 查看客户端提供的协议列表
   - 按照服务器的优先级选择一个
   - 如果没有匹配，使用服务器的默认协议

3. Server → Client:
   TLS ServerHello {
       ...
       ALPN extension: "h2"
   }

4. 连接建立后，双方使用协商的协议通信
```

### Wire Format（线格式）

ALPN 协议列表在 TLS 扩展中的编码格式：

```
长度字节 + 协议名称

示例：["h2", "http/1.1"]

线格式（十六进制）：
02 68 32 08 68 74 74 70 2f 31 2e 31
│  │  │  │  │                      │
│  h  2  │  h  t  t  p  /  1  .  1
│        │
长度=2   长度=8
```

## 常见问题

### Q1: ALPN 和 HTTP/2 Upgrade 有什么区别？

**ALPN (h2)**:
- 用于 HTTPS（TLS）
- 在 TLS 握手期间完成
- 更高效（少一次往返）
- 浏览器标准方式

**HTTP/1.1 Upgrade (h2c)**:
- 用于 HTTP（cleartext）
- 需要额外的 HTTP 请求/响应
- 多一次往返
- 适用于非加密场景

### Q2: 如果客户端不支持 ALPN 会怎样？

服务器会使用配置的默认协议（通常是优先级最高的协议）。

### Q3: 可以只支持 HTTP/2 吗？

可以，使用 `AlpnProtocolList::http2Only()`。但要注意不支持 HTTP/2 的客户端将无法连接。

### Q4: ALPN 配置是全局的吗？

是的，`configureServerAlpn()` 配置的是全局 `SSL_CTX`，影响所有使用该上下文的连接。

### Q5: 如何验证 ALPN 是否生效？

使用 `curl -v --http2` 或 OpenSSL 命令行工具，查看输出中的 ALPN 信息。

## 最佳实践

1. **生产环境**：使用 `AlpnProtocolList::http2WithFallback()`
   - 优先使用 HTTP/2（性能更好）
   - 自动兼容 HTTP/1.1（兼容性好）

2. **微服务**：如果确定所有服务都支持 HTTP/2，可以使用 `AlpnProtocolList::http2Only()`

3. **测试环境**：可以根据需要灵活配置

4. **日志记录**：建议记录协商的协议，便于调试：
   ```cpp
   std::string negotiated = getAlpnProtocol(ssl);
   LOG_INFO("ALPN negotiated: {}", negotiated);
   ```

5. **监控**：统计不同协议的使用情况，了解客户端支持度

## 参考资料

- [RFC 7301 - ALPN](https://tools.ietf.org/html/rfc7301)
- [RFC 7540 - HTTP/2](https://tools.ietf.org/html/rfc7540)
- [OpenSSL ALPN Documentation](https://www.openssl.org/docs/man1.1.1/man3/SSL_CTX_set_alpn_select_cb.html)

## 示例代码

完整的示例代码见：
- `test/test_https_http2_alpn_server.cc` - HTTPS + ALPN 服务器
- `test/test_alpn_config.cc` - ALPN 配置示例

