# B4-H2cClient 压测文档

## 概述

B4-H2cClient 是配合 B3-H2cServer 使用的 HTTP/2 Cleartext 客户端压测程序。

## 测试架构

- **服务器程序**：`benchmark/B3-H2cServer.cc`
- **客户端程序**：`benchmark/B4-H2cClient.cc`
- **测试模式**：HTTP/2 并发请求测试
- **协议**：HTTP/2 over cleartext (h2c)

## 编译

```bash
cd build
cmake --build . --target B4-H2cClient
```

## 使用方法

### 运行客户端压测

```bash
# 基本用法
./build/benchmark/B4-H2cClient

# 自定义参数
./build/benchmark/B4-H2cClient <host> <port> <并发数> <请求数>
```

## 测试指标

- 总请求数
- 成功请求数
- 失败请求数
- 连接成功率
- 平均延迟
- 请求吞吐量 (req/s)

## 相关文件

- `benchmark/B3-H2cServer.cc` - H2c 服务器压测程序
- `benchmark/B4-H2cClient.cc` - H2c 客户端压测程序
