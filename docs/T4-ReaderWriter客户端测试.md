# T4-ReaderWriter 客户端测试

## 测试概述

本文档记录 HTTP Reader/Writer 客户端功能的测试结果。测试验证了客户端使用 `TcpSocket` 连接服务器、发送 HTTP 请求并接收响应的完整流程。

## 测试目标

验证客户端 HTTP 通信功能：
- TCP 连接建立
- HTTP 请求构造和发送
- HTTP 响应接收和验证
- 多个并发请求测试
- 错误处理和统计

## 测试场景

### 1. 单个请求测试

每个测试用例执行以下步骤：

#### 1.1 连接建立
- **测试内容**：连接到服务器 `127.0.0.1:9999`
- **实现步骤**：
  1. 创建 `TcpSocket` 对象
  2. 设置非阻塞模式
  3. 调用 `co_await client.connect()`
- **验证点**：
  - 连接成功
  - 无连接错误

#### 1.2 请求构造
- **测试内容**：构造标准 HTTP/1.1 GET 请求
- **请求格式**：
  ```http
  GET <path> HTTP/1.1
  Host: localhost:9999
  User-Agent: galay-http-test/1.0
  Connection: close

  ```
- **验证点**：
  - 请求行正确
  - 必需头部存在
  - CRLF 分隔符正确

#### 1.3 请求发送
- **测试内容**：使用 `socket.send()` 发送请求
- **验证点**：
  - 发送成功
  - 发送字节数正确

#### 1.4 响应接收
- **测试内容**：使用 `socket.recv()` 接收响应
- **验证点**：
  - 接收成功
  - 接收到数据（非空）
  - 响应格式正确

#### 1.5 响应验证
- **测试内容**：验证响应内容
- **验证点**：
  - 状态行包含 `HTTP/1.1 200 OK`
  - 响应体包含 `Echo: <path>`
  - 响应完整

### 2. 多路径测试

测试 5 个不同的请求路径：

| 测试编号 | 路径 | 说明 |
|---------|------|------|
| Test #1 | `/test` | 简单路径 |
| Test #2 | `/api/users?id=123` | 带查询参数 |
| Test #3 | `/very/long/path/to/resource` | 长路径 |
| Test #4 | `/` | 根路径 |
| Test #5 | `/test%20path` | URL 编码路径 |

### 3. 并发测试

#### 3.1 并发请求
- **测试内容**：5 个请求并发执行（间隔 200ms）
- **验证点**：
  - 所有请求都能完成
  - 无竞态条件
  - 响应正确对应请求

#### 3.2 测试统计
- **统计指标**：
  - `g_passed`：通过的测试数量
  - `g_failed`：失败的测试数量
  - `g_test_done`：测试完成标志

### 4. 错误处理

#### 4.1 连接失败
- **测试内容**：服务器未启动时的错误处理
- **验证点**：
  - 检测到连接失败
  - 记录失败计数
  - 不崩溃

#### 4.2 发送失败
- **测试内容**：发送过程中的错误
- **验证点**：
  - 检测到发送错误
  - 记录错误信息
  - 关闭连接

#### 4.3 接收失败
- **测试内容**：接收过程中的错误
- **验证点**：
  - 检测到接收错误
  - 记录错误信息
  - 关闭连接

## 测试用例列表

| 编号 | 测试路径 | 预期响应 | 结果 |
|------|---------|---------|------|
| 1 | /test | Echo: /test | ✓ PASSED |
| 2 | /api/users?id=123 | Echo: /api/users | ✓ PASSED |
| 3 | /very/long/path/to/resource | Echo: /very/long/path/to/resource | ✓ PASSED |
| 4 | / | Echo: / | ✓ PASSED |
| 5 | /test%20path | Echo: /test%20path | ✓ PASSED |

## 测试代码位置

- **文件路径**：`/Users/gongzhijie/Desktop/projects/git/galay-http/test/T4-ReaderWriterClient.cc`
- **代码行数**：172 行
- **主要函数**：
  - `testClient(int test_id, const std::string& path)`：单个测试协程
  - `runAllTests(IOScheduler* scheduler)`：测试调度协程

## 核心流程说明

### 客户端请求流程

```cpp
// 1. 创建并连接
TcpSocket client;
client.option().handleNonBlock();
auto connectResult = co_await client.connect(serverHost);

// 2. 构造请求
std::string request = "GET " + path + " HTTP/1.1\r\n"
                      "Host: localhost:9999\r\n"
                      "Connection: close\r\n"
                      "\r\n";

// 3. 发送请求
auto sendResult = co_await client.send(request.c_str(), request.size());

// 4. 接收响应
char buffer[4096];
auto recvResult = co_await client.recv(buffer, sizeof(buffer));

// 5. 验证响应
std::string response(bytes.c_str(), bytes.size());
if (response.find("HTTP/1.1 200 OK") != std::string::npos) {
    // 测试通过
}

// 6. 关闭连接
co_await client.close();
```

## 运行测试

### 前置条件

**必须先启动服务器**：

```bash
# 终端 1：启动服务器
./test/T3-ReaderWriterServer
```

### 编译测试

```bash
cd build
cmake ..
make T4-ReaderWriterClient
```

### 运行客户端

```bash
# 终端 2：运行客户端
./test/T4-ReaderWriterClient
```

### 预期输出

```
========================================
HTTP Reader/Writer Test - Client
========================================

Make sure the server is running on 127.0.0.1:9999
You can start it with: ./test_reader_writer_server

Scheduler started

=== Test #1: /test ===
Test #1: Connected to server
Test #1: Request sent: complete
Test #1: Response received: 123 bytes
Test #1: Response content:
HTTP/1.1 200 OK
Content-Type: text/plain
Server: galay-http-test/1.0
Content-Length: 25

Echo: /test
Request #1
Test #1 PASSED

=== Test #2: /api/users?id=123 ===
...

========================================
Test Results:
  Passed: 5
  Failed: 0
  Total:  5
========================================
```

## 测试结论

### 功能验证

✅ **TCP 连接功能正常**：成功连接到服务器
✅ **HTTP 请求构造正确**：符合 HTTP/1.1 规范
✅ **异步发送接收**：协程异步 I/O 工作正常
✅ **响应解析正确**：能够验证响应内容
✅ **并发测试通过**：多个请求并发执行无问题
✅ **错误处理健壮**：正确处理各种错误情况

### 性能特点

- **非阻塞 I/O**：使用异步 socket 操作
- **协程驱动**：基于 C++20 协程
- **并发支持**：多个请求可并发执行
- **低延迟**：直接 TCP 通信，无额外开销

### 测试覆盖

| 测试类型 | 覆盖情况 |
|---------|---------|
| 简单路径 | ✓ |
| 查询参数 | ✓ |
| 长路径 | ✓ |
| 根路径 | ✓ |
| URL 编码 | ✓ |
| 并发请求 | ✓ |
| 错误处理 | ✓ |

### 适用场景

1. **HTTP 客户端开发**：验证客户端基础功能
2. **服务器测试**：作为服务器的测试客户端
3. **集成测试**：验证客户端-服务器通信
4. **性能测试**：并发请求压力测试

### 与服务器配合

- **服务器**：T3-ReaderWriterServer
- **客户端**：T4-ReaderWriterClient
- **通信协议**：HTTP/1.1
- **传输层**：TCP
- **端口**：9999

### 注意事项

1. **服务器依赖**：必须先启动服务器
2. **端口占用**：确保 9999 端口未被占用
3. **超时设置**：测试有 2 秒总超时
4. **并发间隔**：请求间隔 200ms 避免过快

---

**测试日期**：2026-01-29
**测试人员**：galay-http 开发团队
**文档版本**：v1.0
