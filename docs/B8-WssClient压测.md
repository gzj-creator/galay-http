# B8-WssClient 压测文档

## 概述

B8-WssClient 是配合 B7-WssServer 使用的 WebSocket Secure 客户端压测程序。

## 测试架构

- **服务器程序**：`benchmark/B7-WssServer.cc`
- **客户端程序**：`benchmark/B8-WssClient.cc`
- **测试模式**：加密 WebSocket 消息测试
- **协议**：WebSocket over TLS (wss://)

## 编译

```bash
cd build
cmake --build . --target B8-WssClient
```

## 使用方法

### 生成测试证书

```bash
mkdir -p cert
openssl req -x509 -newkey rsa:2048 -keyout cert/test.key -out cert/test.crt \
    -days 365 -nodes -subj "/CN=localhost"
```

### 运行客户端压测

```bash
# 基本用法：<host> <port> <path> <消息数>
./build/benchmark/B8-WssClient localhost 8443 /ws 10

# 并发压测
for i in $(seq 1 100); do
    ./build/benchmark/B8-WssClient localhost 8443 /ws 20 &
done
wait
```

## 测试指标

- 总连接数
- 成功连接数
- 失败连接数
- 总消息数
- 连接速率 (conn/s)
- 消息吞吐量 (msg/s)

## 相关文件

- `benchmark/B7-WssServer.cc` - WSS 服务器压测程序
- `benchmark/B8-WssClient.cc` - WSS 客户端压测程序
