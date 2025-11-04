# HTML 测试页面说明

本目录包含用于测试 HTTP/2 和 WebSocket 的 HTML 页面。

## 📁 文件列表

### HTTP/2 测试页面

| 文件 | 协议 | 端口 | 说明 |
|------|------|------|------|
| `test_h2.html` | HTTP/2 over TLS (h2) | 8443 | 测试 HTTPS + ALPN 协商的 HTTP/2 |
| `test_h2c.html` | HTTP/2 over cleartext (h2c) | 8080 | 测试 HTTP/1.1 Upgrade 到 HTTP/2 |

### WebSocket 测试页面

| 文件 | 协议 | 端口 | 说明 |
|------|------|------|------|
| `test_ws_browser.html` | WebSocket | 8080 | WebSocket 连接测试 |

## 🚀 使用方法

### 测试 HTTP/2 over TLS (h2)

1. 启动 HTTPS 服务器：
```bash
cd build/test
./test_http2_server_h2
```

2. 在浏览器中打开：
```
https://localhost:8443/test_h2.html
```

**注意**：
- 需要 SSL 证书（server.crt 和 server.key）
- 浏览器会显示证书警告（自签名证书），点击"高级"然后"继续访问"
- 浏览器会自动通过 ALPN 协商 HTTP/2

### 测试 HTTP/2 over cleartext (h2c)

1. 启动 HTTP 服务器：
```bash
cd build/test
./test_http2_server_h2c
```

2. 在浏览器中打开：
```
http://localhost:8080/test_h2c.html
```

**注意**：
- 不需要 SSL 证书
- 使用 HTTP/1.1 Upgrade 机制升级到 HTTP/2
- 浏览器对 h2c 的支持有限，建议使用 curl 测试

### 测试 WebSocket

1. 启动 WebSocket 服务器：
```bash
cd build/test
./test_ws_server
```

2. 在浏览器中打开：
```
http://localhost:8080/test_ws_browser.html
```

## 📊 测试功能对比

### test_h2.html (HTTPS + HTTP/2)

- ✅ 安全的 TLS 加密连接
- ✅ ALPN 自动协商
- ✅ 浏览器原生支持
- ✅ 多路复用
- ✅ HPACK 头部压缩
- ✅ 服务器推送支持

**测试项目**：
- 简单请求
- 并发请求（测试多路复用）
- POST 请求
- 大头部测试（测试 HPACK 压缩）
- 压力测试（50个并发请求）

### test_h2c.html (HTTP + HTTP/2)

- ⚠️ 明文传输（无加密）
- ⚠️ 需要 HTTP/1.1 Upgrade
- ⚠️ 浏览器支持有限
- ✅ 多路复用
- ✅ HPACK 头部压缩

**测试项目**：
- 简单请求
- 并发请求（10个）
- 大头部测试
- 流优先级测试

## 🛠️ 技术说明

### h2 vs h2c 的区别

| 特性 | h2 (HTTP/2 over TLS) | h2c (HTTP/2 cleartext) |
|------|---------------------|----------------------|
| 传输层 | TLS 1.2+ | TCP |
| 安全性 | 加密 | 明文 |
| 协商方式 | ALPN (TLS 扩展) | HTTP/1.1 Upgrade |
| 浏览器支持 | 全面支持 | 支持有限 |
| 默认端口 | 443 (HTTPS) | 80 (HTTP) |
| 生产环境 | 推荐 ✅ | 不推荐 ⚠️ |

### ALPN (Application-Layer Protocol Negotiation)

ALPN 是 TLS 扩展，允许客户端和服务器在 TLS 握手期间协商应用层协议：

1. 客户端发送支持的协议列表（如 `h2`, `http/1.1`）
2. 服务器选择一个协议并返回
3. 如果服务器支持 HTTP/2，选择 `h2`
4. 否则降级到 HTTP/1.1

### HTTP/1.1 Upgrade 机制

h2c 使用 HTTP/1.1 的 Upgrade 机制：

```http
GET / HTTP/1.1
Host: localhost:8080
Connection: Upgrade, HTTP2-Settings
Upgrade: h2c
HTTP2-Settings: <base64-encoded-settings>
```

服务器响应：
```http
HTTP/1.1 101 Switching Protocols
Connection: Upgrade
Upgrade: h2c
```

## 📝 测试建议

1. **开发测试**：使用 `test_h2.html` (HTTPS)
   - 更接近生产环境
   - 浏览器支持更好
   - 可以测试完整的 HTTP/2 特性

2. **协议调试**：使用 `test_h2c.html` (HTTP)
   - 可以用 Wireshark 抓包分析
   - 不需要处理 TLS 加密
   - 便于调试 HTTP/2 帧格式

3. **命令行测试**：使用 curl
```bash
# 测试 h2
curl -v --http2 https://localhost:8443/api/hello --insecure

# 测试 h2c
curl -v --http2-prior-knowledge http://localhost:8080/api/test
```

## 🔍 浏览器开发者工具

在 Chrome/Edge 中查看 HTTP/2 连接：

1. 打开开发者工具 (F12)
2. 进入 Network 标签
3. 查看 Protocol 列（可能需要右键添加此列）
4. 应该显示 `h2` (HTTPS) 或 `http/1.1` (HTTP)

## 🐛 常见问题

### Q: 为什么浏览器显示 HTTP/1.1 而不是 h2？

A: 可能的原因：
- 服务器未启用 HTTP/2 支持
- 没有使用 HTTPS（浏览器只在 HTTPS 上支持 HTTP/2）
- ALPN 协商失败
- 浏览器版本过老

### Q: 证书警告怎么办？

A: 测试服务器使用自签名证书，这是正常的。在浏览器中：
1. 点击"高级"或"Advanced"
2. 点击"继续访问"或"Proceed"
3. 或者在 Chrome 中输入 `thisisunsafe`

### Q: h2c 在浏览器中不工作？

A: 这是正常的，大多数浏览器默认不支持 h2c。建议使用 curl 测试：
```bash
curl -v --http2-prior-knowledge http://localhost:8080/
```

## 📚 相关文档

- [RFC 7540 - HTTP/2](https://tools.ietf.org/html/rfc7540)
- [RFC 7541 - HPACK](https://tools.ietf.org/html/rfc7541)
- [RFC 7301 - ALPN](https://tools.ietf.org/html/rfc7301)

