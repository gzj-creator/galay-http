# B2-HttpClient 压测文档

## 概述

B2-HttpClient 是配合 B1-HttpServer 使用的 HTTP 客户端压测程序，用于测试 HTTP 服务器的性能表现。

## 测试架构

- **服务器程序**：`benchmark/B1-HttpServer.cc`
- **客户端程序**：`benchmark/B2-HttpClient.cc`
- **测试模式**：并发请求测试
- **协议**：HTTP/1.1

## 编译

```bash
cd build
cmake --build . --target B2-HttpClient
```

## 使用方法

### 运行客户端压测

```bash
# 基本用法
./build/benchmark/B2-HttpClient

# 自定义参数
./build/benchmark/B2-HttpClient <host> <port> <并发数> <请求数>
```

## 测试指标

- 总请求数
- 成功请求数
- 失败请求数
- 平均延迟
- QPS (每秒请求数)
- 吞吐量

## 相关文件

- `benchmark/B1-HttpServer.cc` - HTTP 服务器压测程序
- `benchmark/B2-HttpClient.cc` - HTTP 客户端压测程序
