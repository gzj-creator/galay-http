# B7-WSS (WebSocket Secure) 压力测试

## 测试环境

- **平台**: macOS Darwin 24.6.0
- **CPU**: Apple Silicon
- **服务器配置**: 4 IO 调度器, 10 计算调度器
- **SSL**: OpenSSL, TLS 1.3
- **证书**: 自签名证书 (RSA 2048)

## 测试结果

### 测试1: 顺序连接测试

| 指标 | 结果 |
|------|------|
| 连接数 | 100 |
| 成功率 | 100% |
| 总耗时 | 21.49s |
| 连接速率 | 4.65 连接/秒 |

### 测试2: 中等并发测试

| 指标 | 结果 |
|------|------|
| 并发客户端 | 50 |
| 每客户端消息数 | 10 |
| 成功率 | 100% (50/50) |
| 总消息数 | 500 |
| 总耗时 | 1.25s |
| 连接速率 | 39.94 连接/秒 |
| 消息吞吐 | 399.43 消息/秒 |

### 测试3: 高并发测试

| 指标 | 结果 |
|------|------|
| 并发客户端 | 100 |
| 每客户端消息数 | 20 |
| 成功率 | 100% (100/100) |
| 总消息数 | 2000 |
| 总耗时 | 1.56s |
| 连接速率 | 64.12 连接/秒 |
| 消息吞吐 | 1282.41 消息/秒 |

### 测试4: 极限压力测试

| 指标 | 结果 |
|------|------|
| 并发客户端 | 200 |
| 每客户端消息数 | 50 |
| 成功率 | 100% (200/200) |
| 总消息数 | 10000 |
| 总耗时 | 6.32s |
| 连接速率 | 31.62 连接/秒 |
| 消息吞吐 | 1581.20 消息/秒 |

## 性能总结

| 测试场景 | 并发数 | 消息吞吐 (msg/s) | 成功率 |
|---------|--------|-----------------|--------|
| 中等并发 | 50 | 399 | 100% |
| 高并发 | 100 | 1282 | 100% |
| 极限压力 | 200 | 1581 | 100% |

## 测试命令

```bash
# 生成测试证书
mkdir -p cert
openssl req -x509 -newkey rsa:2048 -keyout cert/test.key -out cert/test.crt \
    -days 365 -nodes -subj "/CN=localhost"

# 启动 WSS 服务器
./build/example/E7-WssServer 8443 cert/test.crt cert/test.key

# 运行客户端测试
./build/example/E8-WssClient localhost 8443 /ws 10

# 并发压测脚本
for i in $(seq 1 100); do
    ./build/example/E8-WssClient localhost 8443 /ws 20 &
done
wait
```

## 实现说明

### 架构

WSS 实现基于 `HttpsServer` + `WsFrameParser`:

1. **服务器端**: 使用 `HttpsServer` 处理 TLS 连接，在 HTTP 层完成 WebSocket 升级后，直接使用 `SslSocket` 和 `WsFrameParser` 处理 WebSocket 帧
2. **客户端**: 直接使用 `SslSocket` 进行 TCP 连接和 SSL 握手，然后发送 WebSocket 升级请求，使用 `WsFrameParser` 进行帧编解码

### 关键技术点

1. **SSL 非阻塞 I/O**: 正确处理 `SSL_ERROR_WANT_READ` 和 `SSL_ERROR_WANT_WRITE`，遇到时重试而非报错
2. **WebSocket 帧处理**: 使用 `WsFrameParser` 进行帧的编解码，支持 Text、Binary、Ping、Pong、Close 帧
3. **客户端掩码**: 客户端发送的帧必须使用掩码 (`use_mask = true`)

### 限制

由于 `galay-ssl` 的 `SslSocket` 不支持 `readv` 方法，现有的 `WsConn`/`WsReader`/`WsWriter` 模板类不能直接用于 SSL。WSS 实现采用直接操作 `SslSocket` + `WsFrameParser` 的方式。

## 相关文件

- `example/E7-WssServer.cc` - WSS 服务器示例
- `example/E8-WssClient.cc` - WSS 客户端示例
- `galay-http/kernel/websocket/WsUpgrade.h` - WebSocket 升级处理
- `galay-http/protoc/websocket/WebSocketFrame.h` - WebSocket 帧解析器
