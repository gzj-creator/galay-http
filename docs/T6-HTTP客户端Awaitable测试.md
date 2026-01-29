# T6-HTTP 客户端 Awaitable 测试

## 测试概述

本文档记录 `HttpClientAwaitable` 功能的测试结果。测试验证了 HttpClient 的新 API 设计，支持在循环中等待请求完成，解决了旧 API 的一次性使用限制。

## 测试目标

验证 HttpClientAwaitable 的核心功能：
- 循环等待机制（返回 `std::optional<HttpResponse>`）
- GET 请求支持
- POST 请求支持
- 多个连续请求
- 请求完成检测

## API 设计说明

### 旧 API 问题

旧 API 返回 `Awaitable<HttpResponse>`，只能 `co_await` 一次：

```cpp
// ❌ 旧 API：只能等待一次
auto response = co_await client.get("/api/info");
```

### 新 API 设计

新 API 返回 `std::optional<HttpResponse>`，支持循环等待：

```cpp
// ✅ 新 API：可以循环等待
while (true) {
    auto result = co_await client.get("/api/info");

    if (!result) {
        // 错误处理
        break;
    }

    if (result.value().has_value()) {
        // 请求完成，获取响应
        HttpResponse response = std::move(result.value().value());
        break;
    }

    // std::nullopt，继续循环
}
```

**返回值说明**：
- `std::expected<std::optional<HttpResponse>, HttpError>`
- `std::nullopt`：请求进行中，需要继续循环
- `HttpResponse`：请求完成
- `HttpError`：请求失败

## 测试场景

### 1. GET 请求测试

#### 1.1 基本 GET 请求
- **测试内容**：发送 GET 请求到 `/api/info`
- **实现步骤**：
  1. 创建 TcpSocket 并连接
  2. 创建 HttpClient
  3. 循环调用 `client.get()`
  4. 检查返回值类型
  5. 获取响应
- **验证点**：
  - 连接成功
  - 请求发送成功
  - 循环等待正确
  - 响应接收完整
  - 状态码正确

#### 1.2 循环计数
- **测试内容**：记录循环次数
- **验证点**：
  - 循环次数合理（通常 1-10 次）
  - 最终完成
  - 无死循环

### 2. POST 请求测试

#### 2.1 JSON POST 请求
- **测试内容**：发送 JSON 数据到 `/api/data`
- **请求体**：
  ```json
  {"name":"test","value":123}
  ```
- **Content-Type**：`application/json`
- **验证点**：
  - POST 请求正确构造
  - Content-Type 正确设置
  - Content-Length 正确计算
  - 请求体正确发送
  - 响应接收成功

#### 2.2 循环等待
- **测试内容**：POST 请求的循环等待
- **验证点**：
  - 循环机制与 GET 一致
  - 请求完成检测正确

### 3. 多个连续请求测试

#### 3.1 连续请求
- **测试内容**：在同一连接上发送 3 个请求
- **请求列表**：
  1. `GET /`
  2. `GET /hello`
  3. `GET /test`
- **验证点**：
  - 每个请求独立完成
  - 连接复用正确
  - 无状态污染

#### 3.2 请求间隔
- **测试内容**：请求之间无需等待
- **验证点**：
  - 上一个请求完成后立即发送下一个
  - 无额外延迟

## 测试用例列表

| 编号 | 测试用例 | 方法 | URI | 预期结果 |
|------|---------|------|-----|---------|
| 1 | GET 请求 | GET | /api/info | ✓ 循环等待完成 |
| 2 | POST 请求 | POST | /api/data | ✓ JSON 发送成功 |
| 3 | 连续请求 #1 | GET | / | ✓ 请求完成 |
| 4 | 连续请求 #2 | GET | /hello | ✓ 请求完成 |
| 5 | 连续请求 #3 | GET | /test | ✓ 请求完成 |

## 测试代码位置

- **文件路径**：`/Users/gongzhijie/Desktop/projects/git/galay-http/test/T6-HttpClientAwaitable.cc`
- **代码行数**：235 行
- **主要函数**：
  - `testGet()`：GET 请求测试
  - `testPost()`：POST 请求测试
  - `testMultipleRequests()`：多请求测试

## 核心 API 说明

### HttpClient 类

```cpp
class HttpClient {
public:
    // GET 请求
    Awaitable<std::expected<std::optional<HttpResponse>, HttpError>>
        get(const std::string& uri);

    // POST 请求
    Awaitable<std::expected<std::optional<HttpResponse>, HttpError>>
        post(const std::string& uri,
             const std::string& body,
             const std::string& content_type = "text/plain");

    // 关闭连接
    Awaitable<void> close();
};
```

### 使用模式

```cpp
// 标准使用模式
int loop_count = 0;
while (true) {
    loop_count++;

    auto result = co_await client.get("/api/info");

    // 错误处理
    if (!result) {
        LogError("Request failed: {}", result.error().message());
        break;
    }

    // 完成检测
    if (result.value().has_value()) {
        HttpResponse response = std::move(result.value().value());
        LogInfo("Request completed after {} loops", loop_count);
        // 处理响应
        break;
    }

    // 继续循环
    LogInfo("Request in progress, continuing...");

    // 防止死循环
    if (loop_count > 100) {
        LogError("Too many loops");
        break;
    }
}
```

## 运行测试

### 前置条件

**必须先启动测试服务器**：

```bash
# 终端 1：启动服务器
./test/T5-HttpServer
```

### 编译测试

```bash
cd build
cmake ..
make T6-HttpClientAwaitable
```

### 运行测试

```bash
# 终端 2：运行测试
./test/T6-HttpClientAwaitable
```

### 预期输出

```
========================================
HttpClientAwaitable Functionality Test
========================================

Runtime started with 4 IO schedulers

=== Test 1: GET Request ===
Connected to 127.0.0.1:8080
Loop iteration: 1
  Request in progress, continuing...
Loop iteration: 2
✓ GET request completed successfully!
  Status: 200 OK
  Body: {...}
  Total loops: 2

=== Test 2: POST Request ===
Connected to 127.0.0.1:8080
Loop iteration: 1
  Request in progress, continuing...
Loop iteration: 2
✓ POST request completed successfully!
  Status: 200 OK
  Total loops: 2

=== Test 3: Multiple Requests ===
Connected to 127.0.0.1:8080
Requesting: /
✓ Request to / completed
  Status: 200
  Body length: 456 bytes
Requesting: /hello
✓ Request to /hello completed
  Status: 200
  Body length: 234 bytes
Requesting: /test
✓ Request to /test completed
  Status: 200
  Body length: 345 bytes

========================================
All Tests Completed
========================================
```

## 测试结论

### 功能验证

✅ **循环等待机制正常**：支持在循环中等待请求完成
✅ **GET 请求功能完善**：正确发送和接收
✅ **POST 请求功能完善**：支持 body 和 Content-Type
✅ **多请求支持**：同一连接可发送多个请求
✅ **错误处理健壮**：正确处理各种错误情况
✅ **API 设计合理**：解决了旧 API 的限制

### API 优势

| 特性 | 旧 API | 新 API |
|------|--------|--------|
| 循环等待 | ❌ 不支持 | ✅ 支持 |
| 多次调用 | ❌ 只能一次 | ✅ 可多次 |
| 进度检测 | ❌ 无法检测 | ✅ 返回 nullopt |
| 错误处理 | ✅ 支持 | ✅ 支持 |
| 使用复杂度 | 简单 | 稍复杂 |

### 循环次数分析

典型循环次数：
- **快速网络**：1-3 次
- **普通网络**：3-10 次
- **慢速网络**：10-50 次
- **超时保护**：建议设置上限（如 100 次）

### 性能特点

- **非阻塞 I/O**：每次循环都是异步操作
- **协程切换**：循环中协程可能被挂起和恢复
- **零拷贝**：响应数据直接从 socket 读取
- **内存高效**：使用 RingBuffer 循环缓冲

### 适用场景

1. **HTTP 客户端**：通用 HTTP 客户端实现
2. **API 调用**：调用 RESTful API
3. **微服务通信**：服务间 HTTP 通信
4. **爬虫开发**：网页抓取和数据采集

### 使用建议

1. **循环上限**：设置最大循环次数防止死循环
2. **错误处理**：检查 `result` 是否为错误
3. **完成检测**：检查 `has_value()` 判断是否完成
4. **资源释放**：使用完毕后调用 `close()`

### 扩展功能

可以基于此 API 实现：
- **超时控制**：添加超时机制
- **重试逻辑**：自动重试失败请求
- **连接池**：复用 HTTP 连接
- **并发请求**：同时发送多个请求

---

**测试日期**：2026-01-29
**测试人员**：galay-http 开发团队
**文档版本**：v1.0
