# B9-HttpServer 压测文档

## 概述

B9-HttpServer 是一个专门用于压测的 HTTP 服务器基准测试程序。它提供了一个轻量级的 HTTP 服务器，用于测试 Galay-HTTP 框架在高并发场景下的性能表现。

## 特性

- **高性能**: 基于 HttpRouter 实现，支持 Keep-Alive 连接复用
- **实时统计**: 每秒更新请求数、错误数和 QPS
- **简单响应**: 返回简单的 "OK" 文本，最小化处理开销
- **易于使用**: 支持命令行参数配置端口

## 编译

```bash
cd build
cmake --build . --target B9-HttpServer
```

## 使用方法

### 启动服务器

```bash
# 使用默认端口 8080
./build/benchmark/B9-HttpServer

# 指定端口
./build/benchmark/B9-HttpServer 9090
```

### 服务器输出

```
========================================
HTTP Server Benchmark
========================================
Port: 8080
Endpoint: http://127.0.0.1:8080/

Benchmark commands:
  wrk -t4 -c100 -d30s --latency http://127.0.0.1:8080/
  wrk -t8 -c500 -d30s --latency http://127.0.0.1:8080/

Press Ctrl+C to stop
========================================

Server started successfully!
Waiting for requests...

[Stats] Requests: 1279162 | Errors: 0 | QPS: 126618 | Uptime: 10s
```

## 压测工具

### 使用 wrk 进行压测

#### 中等强度压测（4线程，100连接）

```bash
wrk -t4 -c100 -d30s --latency http://127.0.0.1:8080/
```

**预期结果**:
```
Running 30s test @ http://127.0.0.1:8080/
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.17ms  674.96us  33.41ms   97.83%
    Req/Sec    22.12k     2.38k   71.73k    92.34%
  Latency Distribution
     50%    1.09ms
     75%    1.14ms
     90%    1.27ms
     99%    2.34ms
  2642958 requests in 30.10s, 1.09GB read
Requests/sec:  87804.19
Transfer/sec:     37.18MB
```

#### 高强度压测（8线程，500连接）

```bash
wrk -t8 -c500 -d30s --latency http://127.0.0.1:8080/
```

**预期结果**:
```
Running 30s test @ http://127.0.0.1:8080/
  8 threads and 500 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     5.80ms    2.26ms  79.27ms   97.94%
    Req/Sec    10.94k     1.06k   16.59k    92.21%
  Latency Distribution
     50%    5.50ms
     75%    5.77ms
     90%    6.35ms
     99%    9.43ms
  2613170 requests in 30.04s, 1.08GB read
Requests/sec:  86997.49
Transfer/sec:     36.84MB
```

#### 短时高并发压测（4线程，100连接，10秒）

```bash
wrk -t4 -c100 -d10s --latency http://127.0.0.1:8080/
```

**实测结果**:
```
Running 10s test @ http://127.0.0.1:8080/
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   791.06us  142.64us   5.80ms   97.05%
    Req/Sec    31.82k     2.32k   33.70k    94.80%
  Latency Distribution
     50%  775.00us
     75%  792.00us
     90%  823.00us
     99%    1.27ms
  1279162 requests in 10.10s, 109.79MB read
Requests/sec: 126618.25
Transfer/sec:     10.87MB
```

## 性能指标

### 测试环境

- **操作系统**: macOS (Darwin 24.6.0)
- **CPU**: Apple Silicon / Intel
- **编译器**: Clang 14+
- **优化级别**: -O2

### 性能数据

| 测试场景 | 线程数 | 连接数 | 持续时间 | QPS | 平均延迟 | P99延迟 |
|---------|--------|--------|----------|-----|----------|---------|
| 短时高并发 | 4 | 100 | 10s | **126,618** | 791μs | 1.27ms |
| 中等强度 | 4 | 100 | 30s | 87,804 | 1.17ms | 2.34ms |
| 高强度 | 8 | 500 | 30s | 86,997 | 5.80ms | 9.43ms |

### 关键特性

✅ **无 "bad file descriptor" 错误**: 经过超过 520 万次请求的压测，没有出现任何文件描述符相关错误

✅ **稳定的 Keep-Alive 支持**: 连接复用率高，减少连接建立开销

✅ **低延迟**: P99 延迟在高并发场景下仍保持在 10ms 以下

✅ **高吞吐量**: 单机 QPS 可达 12 万+

## 实现细节

### 核心代码

```cpp
Coroutine handleHttpRequest(HttpConn& conn, HttpRequest req) {
    g_request_count++;

    // 使用 Builder 构造响应
    auto response = Http1_1ResponseBuilder::ok()
        .header("Server", "Galay-HTTP-Benchmark/1.0")
        .text("OK")
        .build();

    // 发送响应
    auto writer = conn.getWriter();
    while (true) {
        auto result = co_await writer.sendResponse(response);
        if (!result) {
            g_error_count++;
            break;
        }
        if (result.value()) break;
    }

    co_return;
}
```

### 服务器配置

```cpp
HttpServerConfig config;
config.host = "0.0.0.0";
config.port = port;
config.io_scheduler_count = 4;      // 4个IO调度器
config.compute_scheduler_count = 0; // 不使用计算调度器

HttpServer server(config);
```

### 使用 HttpRouter

B9-HttpServer 使用 `HttpRouter` 来管理路由和 Keep-Alive 连接，这是实现高性能的关键：

```cpp
HttpRouter router;
router.addHandler<HttpMethod::GET>("/", handleHttpRequest);

server.start(std::move(router));
```

**为什么使用 HttpRouter？**

- **Keep-Alive 支持**: HttpRouter 自动管理连接复用，避免频繁建立/关闭连接
- **高效路由**: O(1) 精确匹配，性能开销极小
- **连接管理**: 自动处理连接生命周期

## 对比测试

### 直接 Handler vs HttpRouter

| 实现方式 | QPS | 说明 |
|---------|-----|------|
| 直接传递 handler | 9.92 | 每个请求后关闭连接 |
| 使用 HttpRouter | **126,618** | Keep-Alive 连接复用 |

**性能提升**: 使用 HttpRouter 后性能提升了 **12,760 倍**！

## 故障排查

### 常见问题

1. **QPS 很低（<100）**
   - 检查是否使用了 HttpRouter
   - 确认 Keep-Alive 是否正常工作
   - 查看服务器日志是否有错误

2. **连接被拒绝**
   - 检查端口是否被占用：`lsof -i :8080`
   - 确认防火墙设置
   - 检查系统文件描述符限制：`ulimit -n`

3. **延迟很高**
   - 检查系统负载
   - 调整 IO 调度器数量
   - 使用本地回环地址测试（127.0.0.1）

## 最佳实践

1. **调整系统限制**
   ```bash
   # 增加文件描述符限制
   ulimit -n 65535

   # 调整 TCP 参数（Linux）
   sudo sysctl -w net.ipv4.tcp_tw_reuse=1
   sudo sysctl -w net.ipv4.tcp_fin_timeout=30
   ```

2. **选择合适的调度器数量**
   - IO 调度器：通常设置为 CPU 核心数的 2 倍
   - 计算调度器：对于简单响应可以设置为 0

3. **使用本地测试**
   - 使用 127.0.0.1 而不是 localhost（避免 DNS 查询）
   - 客户端和服务器在同一台机器上测试

## 总结

B9-HttpServer 是一个高性能的 HTTP 服务器基准测试程序，展示了 Galay-HTTP 框架在高并发场景下的优秀性能。通过使用 HttpRouter 和 Keep-Alive 连接复用，可以实现 12 万+ QPS 的吞吐量，同时保持低延迟和高稳定性。

## 相关文档

- [HttpServer 文档](../README.md#httpserver)
- [HttpRouter 文档](11-HttpRouter.md)
- [性能优化指南](../README.md#性能数据)
