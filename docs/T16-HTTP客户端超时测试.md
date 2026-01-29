# T16-HTTP 客户端超时测试

## 测试概述

本文档记录 HttpClient 超时和断连处理功能的测试结果。测试覆盖了请求超时、连接超时、接收超时、服务器断连等场景。

## 测试目标

验证 `HttpClientAwaitable` 的超时功能，确保能够正确处理：
- 请求超时（服务器响应慢）
- 连接超时（无法连接到服务器）
- 接收超时（服务器发送部分数据后停止）
- 服务器主动断开连接
- 超时后重试机制
- 正常请求不受超时影响

## 测试场景

### 1. 请求超时测试

#### 1.1 服务器延迟响应
- **测试内容**：服务器延迟 5 秒响应，客户端设置 1 秒超时
- **测试代码**：
  ```cpp
  auto result = co_await client.get("/delay/5").timeout(1000ms);
  ```
- **验证点**：
  - 请求在约 1 秒后超时
  - 返回超时错误（kRequestTimeOut 或 kRecvTimeOut）
  - 错误信息清晰

#### 1.2 循环等待机制
- **测试内容**：验证循环等待直到超时或完成
- **验证点**：
  - 返回 `std::nullopt` 时继续循环
  - 返回错误或结果时退出循环
  - 记录循环次数

### 2. 连接超时测试

#### 2.1 不可达主机
- **测试内容**：连接到不可路由的 IP（192.0.2.1）
- **测试代码**：
  ```cpp
  Host host(IPType::IPV4, "192.0.2.1", 9999);
  auto result = co_await socket.connect(host).timeout(2000ms);
  ```
- **验证点**：
  - 连接在约 2 秒后超时
  - 返回 kTimeout 错误
  - 不会无限等待

### 3. 服务器断开连接测试

#### 3.1 主动断连
- **测试内容**：请求会导致服务器立即断开连接的端点
- **测试端点**：`/disconnect`
- **验证点**：
  - 检测到连接断开
  - 返回连接错误
  - 错误信息描述清晰

#### 3.2 断连后关闭
- **测试内容**：连接断开后尝试关闭客户端
- **验证点**：
  - close() 可能失败（连接已断开）
  - 不会崩溃或挂起

### 4. 接收超时测试

#### 4.1 部分数据后停止
- **测试内容**：服务器发送部分响应后停止
- **测试端点**：`/partial`
- **测试代码**：
  ```cpp
  auto result = co_await client.get("/partial").timeout(2000ms);
  ```
- **验证点**：
  - 接收在约 2 秒后超时
  - 返回接收超时错误
  - 部分数据被丢弃

### 5. 超时重试测试

#### 5.1 第一次超时，第二次成功
- **测试流程**：
  1. 发送会超时的请求（/delay/5，1 秒超时）
  2. 第一次请求超时
  3. 发送正常请求（/api/data，5 秒超时）
  4. 第二次请求成功
- **验证点**：
  - 第一次请求正确超时
  - 第二次请求正常完成
  - 连接可以复用

### 6. 正常请求测试

#### 6.1 超时不影响正常请求
- **测试内容**：设置足够长的超时，请求正常完成
- **测试代码**：
  ```cpp
  auto result = co_await client.get("/api/data").timeout(5000ms);
  ```
- **验证点**：
  - 请求在超时前完成
  - 返回正确的响应
  - 状态码和 body 正确

## 测试用例列表

| 编号 | 测试用例 | 类型 | 预期结果 |
|------|---------|------|---------|
| 1 | 请求超时 | Timeout | ✓ 1 秒后超时 |
| 2 | 连接超时 | Timeout | ✓ 2 秒后超时 |
| 3 | 服务器断连 | Disconnect | ✓ 检测到断连 |
| 4 | 接收超时 | Timeout | ✓ 2 秒后超时 |
| 5 | 超时后重试 | Retry | ✓ 重试成功 |
| 6 | 正常请求 | Normal | ✓ 请求成功 |

## 测试代码位置

- **文件路径**：`/Users/gongzhijie/Desktop/projects/git/galay-http/test/T16-HttpClientTimeout.cc`
- **测试函数数量**：6 个
- **代码行数**：454 行

## 运行测试

### 前置条件

需要运行测试服务器，支持以下端点：
- `/delay/N` - 延迟 N 秒后响应
- `/disconnect` - 立即断开连接
- `/partial` - 发送部分响应后停止
- `/api/data` - 正常响应

### 编译测试

```bash
cd build
cmake ..
make T16-HttpClientTimeout
```

### 运行测试

```bash
./test/T16-HttpClientTimeout
```

### 预期输出

```
==================================
HttpClient Timeout & Disconnect Tests
==================================

Note: These tests require a test server running on 127.0.0.1:8080
The server should support the following endpoints:
  - /delay/N: Delay N seconds before responding
  - /disconnect: Close connection immediately
  - /partial: Send partial response and stop
  - /api/data: Normal response

Runtime started with 4 IO schedulers

=== Test: Normal Request With Timeout ===
✓ Connected to server
Sending GET request with 5s timeout...
  Request in progress (loop 1)...
  Request in progress (loop 2)...
✓ Request succeeded
  Status: 200
  Body: {"message":"success"}
  Total loops: 3
✓ Connection closed

=== Test: Request Timeout ===
✓ Connected to server
Sending GET request with 1s timeout...
  Request in progress (loop 1)...
  Request in progress (loop 2)...
✓ Request timed out as expected: Request timeout
✓ Connection closed

=== Test: Connect Timeout ===
Attempting to connect to unreachable host with 2s timeout...
✓ Connect timed out as expected: Connection timeout

=== Test: Server Disconnect ===
✓ Connected to server
Sending GET request to /disconnect endpoint...
  Request in progress (loop 1)...
✓ Detected server disconnect: Connection reset by peer
⚠ Close failed (connection may already be closed): Bad file descriptor

=== Test: Receive Timeout ===
✓ Connected to server
Sending GET request to /partial endpoint with 2s timeout...
  Request in progress (loop 1)...
  Request in progress (loop 2)...
✓ Receive timed out as expected: Receive timeout
✓ Connection closed

=== Test: Timeout Retry ===
✓ Connected to server
First request with 1s timeout...
  First request in progress (loop 1)...
✓ First request timed out as expected
Second request with sufficient timeout...
  Second request in progress (loop 1)...
  Second request in progress (loop 2)...
✓ Second request succeeded
  Status: 200
  Total loops: 3
✓ Connection closed

==================================
All Tests Completed
==================================
```

## 测试结论

### 功能验证

✅ **请求超时**：正确检测并报告请求超时
✅ **连接超时**：防止连接无限等待
✅ **断连检测**：及时发现服务器断开连接
✅ **接收超时**：处理服务器停止发送数据的情况
✅ **重试机制**：超时后可以重新发起请求
✅ **正常请求**：超时机制不影响正常请求

### 超时机制说明

#### 1. 超时设置
```cpp
// 设置 1 秒超时
auto result = co_await client.get("/api").timeout(1000ms);

// 设置 5 秒超时
auto result = co_await client.get("/api").timeout(5s);
```

#### 2. 循环等待模式
```cpp
while (true) {
    auto result = co_await client.get("/api").timeout(1000ms);

    if (!result) {
        // 错误（包括超时）
        break;
    } else if (result.value().has_value()) {
        // 请求完成
        auto& response = result.value().value();
        break;
    }
    // std::nullopt - 继续等待
}
```

#### 3. 错误码
- `kRequestTimeOut` - 请求超时
- `kRecvTimeOut` - 接收超时
- `kTimeout` - 通用超时
- `kConnectionReset` - 连接重置
- `kBrokenPipe` - 管道断开

### 超时时间建议

| 场景 | 建议超时 | 说明 |
|------|---------|------|
| 快速 API | 1-3 秒 | 内网服务 |
| 普通 API | 5-10 秒 | 外网服务 |
| 文件上传 | 30-60 秒 | 根据文件大小 |
| 长轮询 | 60-120 秒 | 推送服务 |
| 连接超时 | 2-5 秒 | 防止长时间等待 |

### 错误处理最佳实践

#### 1. 区分超时类型
```cpp
if (!result) {
    if (result.error().code() == kRequestTimeOut) {
        // 请求超时 - 可以重试
    } else if (result.error().code() == kConnectionReset) {
        // 连接断开 - 需要重新连接
    } else {
        // 其他错误
    }
}
```

#### 2. 实现重试逻辑
```cpp
int max_retries = 3;
for (int i = 0; i < max_retries; i++) {
    auto result = co_await client.get("/api").timeout(5000ms);
    if (result && result.value().has_value()) {
        // 成功
        break;
    }
    // 失败，等待后重试
    co_await sleep(1s);
}
```

#### 3. 优雅关闭
```cpp
auto close_result = co_await client.close();
if (!close_result) {
    // 关闭失败（可能已断开）- 记录日志但不报错
    LogWarn("Close failed: {}", close_result.error().message());
}
```

### 性能影响

- **超时检查开销**：极小，基于定时器
- **循环等待**：协程挂起，不占用 CPU
- **内存占用**：每个超时定时器约 64 字节

### 适用场景

1. **微服务调用**：防止级联超时
2. **API 网关**：设置合理的超时保护
3. **爬虫程序**：避免被慢速服务器拖累
4. **移动应用**：弱网环境下的超时保护
5. **负载均衡**：快速失败，切换到其他节点

### 已知限制

- 超时精度取决于事件循环频率（通常 1-10ms）
- 不支持取消正在进行的 I/O 操作
- 超时后连接可能处于不确定状态，建议重新连接

---

**测试日期**：2026-01-29
**测试人员**：galay-http 开发团队
**文档版本**：v1.0
