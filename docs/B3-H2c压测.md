# B3-H2c (HTTP/2 Cleartext) 压力测试

## 测试环境

- **平台**: macOS Darwin 24.6.0
- **CPU**: Apple Silicon
- **服务器程序**: `benchmark/B3-H2cServer.cc`
- **客户端程序**: `benchmark/B4-H2cClient.cc`
- **服务器配置**: 4 IO 调度器
- **客户端配置**: 4 IO 调度器
- **协议**: HTTP/2 over cleartext (h2c)

## 测试结果

### 测试1: 低并发测试

| 指标 | 结果 |
|------|------|
| 并发客户端 | 10 |
| 每客户端请求数 | 10 |
| 连接成功率 | 100% (10/10) |
| 请求成功数 | 100 |
| 请求失败数 | 0 |
| 总耗时 | 1.005s |
| 连接速率 | 9 conn/s |
| 请求吞吐 | 99 req/s |

### 测试2: 中等并发测试

| 指标 | 结果 |
|------|------|
| 并发客户端 | 50 |
| 每客户端请求数 | 200 |
| 连接成功率 | 100% (50/50) |
| 总请求数 | 10000 |
| 请求成功数 | 10000 |
| 总耗时 | 1.004s |
| 连接速率 | 49 conn/s |
| 请求吞吐 | 9960 req/s |

### 测试3: 高并发测试

| 指标 | 结果 |
|------|------|
| 并发客户端 | 100 |
| 每客户端请求数 | 100 |
| 连接成功率 | 100% (100/100) |
| 总请求数 | 10000 |
| 请求成功数 | 10000 |
| 总耗时 | 1.005s |
| 连接速率 | 99 conn/s |
| 请求吞吐 | 9950 req/s |

### 测试4: 极限压力测试

| 指标 | 结果 |
|------|------|
| 并发客户端 | 200 |
| 每客户端请求数 | 100 |
| 连接成功率 | 100% (200/200) |
| 总请求数 | 20000 |
| 请求成功数 | 20000 |
| 总耗时 | 1.005s |
| 连接速率 | 199 conn/s |
| 请求吞吐 | 19900 req/s |

## 性能总结

| 测试场景 | 并发数 | 总请求数 | 请求吞吐 (req/s) | 成功率 |
|---------|--------|---------|-----------------|--------|
| 低并发 | 10 | 100 | 99 | 100% |
| 中等并发 | 50 | 10000 | 9,960 | 100% |
| 高并发 | 100 | 10000 | 9,950 | 100% |
| 极限压力 | 200 | 20000 | 19,900 | 100% |

## 测试命令

```bash
# 启动 H2c 服务器
./build/benchmark/B3-H2cServer 9080 4 [debug]

# 示例（开启日志）
./build/benchmark/B3-H2cServer 9080 4 1

# 运行客户端压测
./build/benchmark/B4-H2cClient <host> <port> <并发数> <每客户端请求数> [最大等待秒数]

# 示例
./build/benchmark/B4-H2cClient localhost 9080 100 50 60
```

## 实现说明

### 架构

H2c 实现基于 HTTP/1.1 Upgrade 机制：

1. **服务器端**:
   - 支持 HTTP/2 Prior Knowledge 模式（直接发送 Connection Preface）
   - 支持 HTTP/1.1 Upgrade 模式（发送 Upgrade: h2c 头部）
   - 使用 `Http2ConnImpl<TcpSocket>` 处理 HTTP/2 连接

2. **客户端**:
   - 使用 `H2cClient` 进行连接和升级
   - 发送 HTTP/1.1 Upgrade 请求，包含 `HTTP2-Settings` 头部
   - 接收 101 Switching Protocols 响应
   - 发送 HTTP/2 Connection Preface 和 SETTINGS 帧
   - 使用 `Http2ConnImpl<TcpSocket>` 处理后续通信

### 关键技术点

1. **HTTP/1.1 Upgrade**:
   - 客户端发送 `Upgrade: h2c` 和 `HTTP2-Settings` 头部
   - 服务器响应 `101 Switching Protocols`
   - 双方切换到 HTTP/2 协议

2. **HTTP/2 握手**:
   - 客户端发送 Connection Preface (`PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n`)
   - 双方交换 SETTINGS 帧
   - 双方发送 SETTINGS ACK

3. **多路复用**:
   - 单个 TCP 连接支持多个并发流
   - 客户端使用奇数流 ID (1, 3, 5, ...)
   - 服务器使用偶数流 ID (2, 4, 6, ...)

4. **HPACK 压缩**:
   - 使用 HPACK 算法压缩 HTTP 头部
   - 维护动态表和静态表
   - 减少头部传输开销

### 与 HTTP/1.1 的对比

| 特性 | HTTP/1.1 | HTTP/2 (h2c) |
|------|----------|--------------|
| 连接复用 | 否（需要 Keep-Alive） | 是（多路复用） |
| 头部压缩 | 否 | 是（HPACK） |
| 服务器推送 | 否 | 是 |
| 优先级 | 否 | 是 |
| 流控制 | 否 | 是 |

## 限制

1. **服务器支持**: 需要服务器支持 HTTP/1.1 Upgrade 或 Prior Knowledge 模式
2. **TLS**: h2c 不使用 TLS，适用于内网或开发环境
3. **浏览器支持**: 大多数浏览器不支持 h2c，只支持 h2 (HTTP/2 over TLS)

## 相关文件

- `benchmark/B8-H2cBenchmark.cc` - H2c 压力测试程序
- `example/E9-H2cEchoServer.cc` - H2c Echo 服务器示例
- `example/E10-H2cEchoClient.cc` - H2c Echo 客户端示例
- `galay-http/kernel/http2/H2cClient.h` - H2c 客户端实现
- `galay-http/kernel/http2/Http2Server.h` - HTTP/2 服务器实现
- `galay-http/kernel/http2/Http2Conn.h` - HTTP/2 连接管理
- `galay-http/protoc/http2/Http2Frame.h` - HTTP/2 帧定义
- `galay-http/protoc/http2/Http2Hpack.h` - HPACK 压缩实现

## 参考资料

- [RFC 7540 - HTTP/2](https://tools.ietf.org/html/rfc7540)
- [RFC 7541 - HPACK](https://tools.ietf.org/html/rfc7541)
- [HTTP/2 Cleartext (h2c)](https://http2.github.io/http2-spec/#discover-http)
