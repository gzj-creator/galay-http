# B6-WebsocketClient 压测文档

## 概述

B6-WebsocketClient 是配合 B5-WebsocketServer 使用的 WebSocket 客户端压测程序。

## 测试架构

- **服务器程序**：`benchmark/B5-WebsocketServer.cc`
- **客户端程序**：`benchmark/B6-WebsocketClient.cc`
- **测试模式**：Echo 消息测试
- **协议**：WebSocket over HTTP/1.1

## 编译

```bash
cd build
cmake --build . --target B6-WebsocketClient
```

## 使用方法

### 运行客户端压测

```bash
# 基本用法：<并发数> <持续时间(秒)> <消息大小(字节)>
./build/benchmark/B6-WebsocketClient 10 10 1024

# 小消息高频测试
./build/benchmark/B6-WebsocketClient 50 10 256

# 大消息传输测试
./build/benchmark/B6-WebsocketClient 20 10 4096

# 高并发测试
./build/benchmark/B6-WebsocketClient 100 30 1024
```

## 测试指标

- 总连接数
- 成功连接数
- 失败连接数
- 总消息数
- 平均延迟
- QPS (消息/秒)
- 吞吐量 (MB/s)

## 相关文件

- `benchmark/B5-WebsocketServer.cc` - WebSocket 服务器压测程序
- `benchmark/B6-WebsocketClient.cc` - WebSocket 客户端压测程序
