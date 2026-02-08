# B4-H2cClient 压测文档

## 概述

B4-H2cClient 是配合 B3-H2cServer 使用的 HTTP/2 Cleartext 客户端压测程序。每个客户端通过 HTTP/1.1 Upgrade 升级到 HTTP/2，利用多路复用在单连接上串行发送多个 Echo 请求。

## 测试架构

- **服务器程序**：`benchmark/B3-H2cServer.cc`
- **客户端程序**：`benchmark/B4-H2cClient.cc`
- **测试模式**：HTTP/2 Echo 请求（发送 14 字节 payload，验证响应一致）
- **协议**：HTTP/2 over cleartext (h2c)，HTTP/1.1 Upgrade 方式

## 编译

```bash
cd build
cmake --build . --target B3-H2cServer B4-H2cClient
```

## 使用方法

```bash
# 启动服务器（4 IO 线程，关闭日志）
./build/benchmark/B3-H2cServer 9080 4 0

# 运行客户端压测
./build/benchmark/B4-H2cClient <host> <port> <并发数> <每客户端请求数> [最大等待秒数]

# 示例
./build/benchmark/B4-H2cClient localhost 9080 100 50 60
```

## 测试结果

| 测试场景 | 并发连接 | 每连接请求数 | 总请求数 | 吞吐 (req/s) | 成功率 |
|---------|---------|------------|---------|-------------|--------|
| 低并发 | 10 | 10 | 100 | 99 | 100% |
| 中等并发 | 50 | 200 | 10,000 | 9,960 | 100% |
| 高并发 | 100 | 100 | 10,000 | 9,950 | 100% |
| 极限压力 | 200 | 100 | 20,000 | 19,900 | 100% |

## 测试指标

- 连接成功率
- 请求成功数 / 失败数
- 请求吞吐量 (req/s)
- 连接速率 (conn/s)

## 相关文件

- `benchmark/B3-H2cServer.cc` - H2c Echo 服务器压测程序
- `benchmark/B4-H2cClient.cc` - H2c Echo 客户端压测程序
