# 使用 Galay 框架的 HTTP/2 支持

## 发现

galay 框架已经提供了 HTTP/2 环境初始化函数！

### API 接口

```cpp
// galay/common/Common.h
namespace galay {
    /**
     * @brief 初始化HTTP/2服务器环境
     * @param cert_file 证书文件路径
     * @param key_file 私钥文件路径
     * @return 初始化是否成功
     */
    bool initializeHttp2ServerEnv(const char* cert_file, const char* key_file);
    
    /**
     * @brief 初始化HTTP/2客户端环境
     * @param server_pem 服务器证书文件路径（可选）
     * @return 初始化是否成功
     */
    bool initializeHttp2ClientEnv(const char* server_pem = nullptr);
    
    /**
     * @brief 销毁HTTP/2环境
     * @return 销毁是否成功
     */
    bool destroyHttp2Env();

    /**
     * @brief 获取全局SSL上下文
     * @return SSL_CTX指针
     */
    SSL_CTX* getGlobalSSLCtx();
}
```

### 当前实现分析

```cpp
// galay/common/Common.cc
bool initializeHttp2ServerEnv(const char* cert_file, const char* key_file)
{
    if(!initializeSSLServerEnv(cert_file, key_file)) {
        return false;
    }
    const unsigned char alpn_protocols[] = "\x08\x04\x00\x00"; // HTTP/2
    SSL_CTX_set_alpn_protos(SslCtx, alpn_protocols, sizeof(alpn_protocols));
    return true;
}
```

#### ⚠️ 存在的问题

1. **ALPN 协议格式不正确**
   ```cpp
   const unsigned char alpn_protocols[] = "\x08\x04\x00\x00"; // ❌ 错误
   ```
   
   正确的格式应该是：
   ```cpp
   // h2 的正确格式：长度(2) + "h2"
   const unsigned char alpn_protocols[] = "\x02h2";
   
   // 或者同时支持 h2 和 http/1.1：
   const unsigned char alpn_protocols[] = "\x02h2\x08http/1.1";
   ```

2. **使用了客户端 API**
   ```cpp
   SSL_CTX_set_alpn_protos(SslCtx, ...);  // ❌ 这是客户端 API
   ```
   
   服务器应该使用：
   ```cpp
   SSL_CTX_set_alpn_select_cb(SslCtx, alpn_callback, nullptr);  // ✅ 服务器 API
   ```

## 修正方案

### 方案 1：在 galay 框架中修复（推荐）

向 galay 框架提交 PR，修正 `initializeHttp2ServerEnv`：

```cpp
// galay/common/Common.cc

// ALPN 选择回调函数
static int alpn_select_callback(SSL *ssl,
                               const unsigned char **out,
                               unsigned char *outlen,
                               const unsigned char *in,
                               unsigned int inlen,
                               void *arg)
{
    // 服务器支持的协议列表
    static const unsigned char server_protos[] = {
        2, 'h', '2',           // HTTP/2
        8, 'h', 't', 't', 'p', '/', '1', '.', '1'  // HTTP/1.1
    };
    
    // 选择协议（优先选择 h2）
    if (SSL_select_next_proto((unsigned char **)out, outlen,
                             server_protos, sizeof(server_protos),
                             in, inlen) == OPENSSL_NPN_NEGOTIATED)
    {
        return SSL_TLSEXT_ERR_OK;
    }
    
    // 没有匹配的协议，使用默认的 http/1.1
    *out = server_protos + 3;  // 跳过 h2，指向 http/1.1
    *outlen = 8;
    return SSL_TLSEXT_ERR_OK;
}

bool initializeHttp2ServerEnv(const char* cert_file, const char* key_file)
{
    if(!initializeSSLServerEnv(cert_file, key_file)) {
        return false;
    }
    
    // 设置 ALPN 回调（服务器端）
    SSL_CTX_set_alpn_select_cb(SslCtx, alpn_select_callback, nullptr);
    
    return true;
}
```

### 方案 2：在 galay-http 中使用现有 API

即使 galay 的实现有问题，我们仍然可以使用它：

```cpp
// test/test_https_http2_server.cc
#include <galay/common/Common.h>
#include "galay-http/server/HttpsServer.h"

int main()
{
    // 1. 初始化 HTTP/2 环境（包括 SSL + ALPN）
    if (!galay::initializeHttp2ServerEnv("server.crt", "server.key")) {
        std::cerr << "Failed to initialize HTTP/2 environment" << std::endl;
        return 1;
    }
    
    // 2. 获取全局 SSL 上下文（可选，用于高级配置）
    SSL_CTX* ctx = galay::getGlobalSSLCtx();
    if (ctx) {
        // 可以进行额外的 SSL 配置
        // SSL_CTX_set_options(ctx, ...);
    }
    
    // 3. 创建 HTTPS 服务器
    HttpsServerBuilder builder("server.crt", "server.key");
    HttpsServer server = builder.build();
    server.listen(Host("0.0.0.0", 8443));
    
    // 4. 运行服务器
    RuntimeBuilder runtimebuilder;
    auto runtime = runtimebuilder.build();
    runtime.start();
    
    HttpRouter router;
    // ... 设置路由
    
    server.run(runtime, router);
    server.wait();
    
    // 5. 清理
    galay::destroyHttp2Env();
    
    return 0;
}
```

### 方案 3：直接修正全局 SSL 上下文

在我们的代码中修正 galay 的 ALPN 配置：

```cpp
#include <galay/common/Common.h>
#include <openssl/ssl.h>

// ALPN 回调函数
int our_alpn_select_cb(SSL *ssl,
                      const unsigned char **out,
                      unsigned char *outlen,
                      const unsigned char *in,
                      unsigned int inlen,
                      void *arg)
{
    static const unsigned char protos[] = {
        2, 'h', '2',           // HTTP/2
        8, 'h', 't', 't', 'p', '/', '1', '.', '1'  // HTTP/1.1
    };
    
    if (SSL_select_next_proto((unsigned char **)out, outlen,
                             protos, sizeof(protos),
                             in, inlen) == OPENSSL_NPN_NEGOTIATED)
    {
        return SSL_TLSEXT_ERR_OK;
    }
    
    return SSL_TLSEXT_ERR_NOACK;
}

int main()
{
    // 初始化 SSL 环境
    galay::initializeSSLServerEnv("server.crt", "server.key");
    
    // 获取全局 SSL 上下文并正确配置 ALPN
    SSL_CTX* ctx = galay::getGlobalSSLCtx();
    if (ctx) {
        // 重新设置正确的 ALPN 回调
        SSL_CTX_set_alpn_select_cb(ctx, our_alpn_select_cb, nullptr);
    }
    
    // 继续创建服务器...
}
```

## 检测 ALPN 协商结果

在连接建立后，可以检查协商的协议：

```cpp
// 在 AsyncSslSocket 中添加方法
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

然后在 `HttpsServer::handleConnection` 中使用：

```cpp
Coroutine<nil> HttpsServer::handleConnection(Runtime& runtime, HttpRouter& router, 
                                            HttpSettings params, AsyncSslSocket socket)
{
    AsyncFactory factory = runtime.getAsyncFactory();
    
    // 检查协商的协议
    std::string protocol = socket.getNegotiatedProtocol();
    
    if (protocol == "h2") {
        HTTP_LOG_INFO("[HttpsServer] Using HTTP/2 (via ALPN)");
        
        // 直接使用 HTTP/2
        Http2Connection http2Conn(std::move(socket), factory.getTimerGenerator());
        // 处理 HTTP/2...
        
    } else {
        HTTP_LOG_INFO("[HttpsServer] Using HTTP/1.1");
        
        // 使用 HTTP/1.1
        HttpsConnection conn(std::move(socket), factory.getTimerGenerator());
        // 处理 HTTP/1.1（可以 Upgrade 到 h2）...
    }
    
    co_return nil();
}
```

## 测试 ALPN

### 使用 OpenSSL 命令行

```bash
# 测试服务器的 ALPN 支持
openssl s_client -connect localhost:8443 -alpn h2 -servername localhost

# 查看输出中的 ALPN 协商结果：
# ALPN protocol: h2
```

### 使用 curl

```bash
# curl 会自动使用 ALPN
curl -v --http2 https://localhost:8443/

# 输出：
# * ALPN, offering h2
# * ALPN, offering http/1.1
# * ALPN, server accepted to use h2
```

## 推荐的实现步骤

1. **短期**：使用方案 3，在我们的代码中修正 ALPN 配置
   ```cpp
   galay::initializeSSLServerEnv(...);
   SSL_CTX* ctx = galay::getGlobalSSLCtx();
   SSL_CTX_set_alpn_select_cb(ctx, our_alpn_select_cb, nullptr);
   ```

2. **中期**：向 galay 框架提交 PR，修正 `initializeHttp2ServerEnv`
   - 修正 ALPN 协议格式
   - 使用 `SSL_CTX_set_alpn_select_cb` 而不是 `SSL_CTX_set_alpn_protos`

3. **长期**：在 `AsyncSslSocket` 中添加 `getNegotiatedProtocol()` 方法
   - 在 `HttpsServer` 中自动检测协议
   - 根据协商结果选择 HTTP/2 或 HTTP/1.1

## ALPN 协议字符串格式

### 正确的格式

ALPN 协议列表的格式是：`长度字节 + 协议名`

```cpp
// h2
"\x02h2"
// 解释：\x02 = 2（长度），"h2" = 协议名

// http/1.1
"\x08http/1.1"
// 解释：\x08 = 8（长度），"http/1.1" = 协议名

// 同时支持两个协议
"\x02h2\x08http/1.1"
// 或者以数组形式
const unsigned char protos[] = {
    2, 'h', '2',
    8, 'h', 't', 't', 'p', '/', '1', '.', '1'
};
```

### galay 框架中的错误格式

```cpp
const unsigned char alpn_protocols[] = "\x08\x04\x00\x00";
// 这会被解析为：
// \x08 = 长度 8
// \x04\x00\x00 = 3 字节数据 + 尝试读取更多...
// 这不是有效的协议名！
```

## 总结

- ✅ galay 框架提供了 `initializeHttp2ServerEnv` API
- ⚠️ 当前实现有 ALPN 格式和 API 使用错误
- 🔧 可以通过获取 `getGlobalSSLCtx()` 并重新配置来修正
- 📝 建议向 galay 框架提交 PR 修正这个问题

## 参考代码

完整的示例代码见：
- `test/test_https_http2_alpn_server.cc` (待创建)

