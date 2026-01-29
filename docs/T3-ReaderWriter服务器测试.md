# T3-ReaderWriter 服务器测试

## 测试概述

本文档记录 HTTP Reader/Writer 服务器端功能的测试结果。测试验证了 `HttpReader` 和 `HttpWriter` 类在服务器场景下的正确性，包括请求读取和响应发送的多种方式。

## 测试目标

验证服务器端 HTTP Reader/Writer 的功能：
- 使用 `HttpReader` 读取客户端请求
- 使用 `HttpWriter` 发送响应的三种方式
- 异步 I/O 与协程的集成
- 连接管理和错误处理

## 测试场景

### 1. 服务器基础功能

#### 1.1 服务器启动和监听
- **测试内容**：启动 TCP 服务器并监听端口 9999
- **验证点**：
  - Socket 选项设置成功（ReuseAddr、NonBlock）
  - 绑定地址成功
  - 监听成功
  - 接受连接成功

#### 1.2 请求读取
- **测试内容**：使用 `HttpReader.getRequest()` 读取 HTTP 请求
- **验证点**：
  - 异步读取数据
  - 增量解析请求
  - 请求完整性检测
  - 请求信息正确（方法、URI）

### 2. 响应发送方式测试

服务器根据请求编号（`g_request_count % 3`）使用不同的发送方式：

#### 2.1 方式 1：sendResponse（完整响应）
- **测试内容**：使用 `writer.sendResponse(response)` 发送完整响应
- **实现步骤**：
  1. 构造 `HttpResponse` 对象
  2. 设置响应头（状态码、Content-Type、Content-Length）
  3. 设置响应体
  4. 调用 `sendResponse()` 一次性发送
- **验证点**：
  - 响应头正确
  - 响应体正确
  - 一次性发送成功

#### 2.2 方式 2：sendHeader + send(string)（分离发送）
- **测试内容**：先发送头部，再发送 body（字符串）
- **实现步骤**：
  1. 构造 `HttpResponseHeader` 对象
  2. 调用 `writer.sendHeader()` 发送头部
  3. 调用 `writer.send(body)` 发送 body
- **验证点**：
  - 头部和 body 分离发送
  - 两次发送都成功
  - 客户端接收完整

#### 2.3 方式 3：send(buffer, length)（原始数据）
- **测试内容**：发送原始字节数据
- **实现步骤**：
  1. 将响应头转换为字符串
  2. 调用 `writer.send(data, length)` 发送头部
  3. 调用 `writer.send(data, length)` 发送 body
- **验证点**：
  - 原始数据发送成功
  - 客户端正确解析

### 3. 响应内容测试

#### 3.1 Echo 响应
- **测试内容**：响应包含客户端请求的 URI
- **响应格式**：
  ```
  Echo: <URI>
  Request #<count>
  ```
- **验证点**：
  - URI 正确回显
  - 请求计数正确

#### 3.2 响应头设置
- **测试内容**：设置标准响应头
- **响应头**：
  - `Content-Type: text/plain`
  - `Server: galay-http-test/1.0`
  - `Content-Length: <body-size>`
- **验证点**：
  - 所有头部字段正确
  - Content-Length 与实际 body 大小匹配

### 4. 连接管理

#### 4.1 连接关闭
- **测试内容**：请求处理完成后关闭连接
- **验证点**：
  - 调用 `client.close()` 成功
  - 资源正确释放

#### 4.2 错误处理
- **测试内容**：处理客户端断开连接
- **验证点**：
  - 检测到 `kConnectionClose` 错误
  - 优雅处理断开

## 测试用例列表

| 编号 | 测试用例 | 发送方式 | 预期结果 |
|------|---------|---------|---------|
| 1 | 服务器启动 | - | ✓ 监听成功 |
| 2 | 接受连接 | - | ✓ 连接建立 |
| 3 | 读取请求 | HttpReader | ✓ 请求解析成功 |
| 4 | 完整响应发送 | sendResponse | ✓ 一次性发送 |
| 5 | 分离发送 | sendHeader + send | ✓ 分两次发送 |
| 6 | 原始数据发送 | send(buffer) | ✓ 原始发送 |
| 7 | Echo 功能 | - | ✓ URI 回显 |
| 8 | 连接关闭 | - | ✓ 优雅关闭 |

## 测试代码位置

- **文件路径**：`/Users/gongzhijie/Desktop/projects/git/galay-http/test/T3-ReaderWriterServer.cc`
- **代码行数**：241 行
- **主要函数**：
  - `echoServer()`：服务器主协程
  - 支持 Kqueue、Epoll、IOUring 三种调度器

## 核心 API 说明

### HttpReader API

```cpp
// 读取 HTTP 请求（增量）
Awaitable<std::expected<bool, HttpError>> getRequest(HttpRequest& request);
```

**返回值**：
- `true`：请求完整
- `false`：需要继续读取
- `error`：解析错误或连接关闭

### HttpWriter API

```cpp
// 方式 1：发送完整响应
Awaitable<std::expected<ssize_t, HttpError>> sendResponse(HttpResponse& response);

// 方式 2：发送响应头
Awaitable<std::expected<ssize_t, HttpError>> sendHeader(HttpResponseHeader&& header);

// 方式 3：发送字符串
Awaitable<std::expected<ssize_t, HttpError>> send(std::string&& data);

// 方式 4：发送原始数据
Awaitable<std::expected<ssize_t, HttpError>> send(const char* buffer, size_t length);
```

## 运行测试

### 编译测试

```bash
cd build
cmake ..
make T3-ReaderWriterServer
```

### 运行服务器

```bash
./test/T3-ReaderWriterServer
```

### 预期输出

```
========================================
HTTP Reader/Writer Test - Server
========================================

=== HTTP Reader/Writer Test Server ===
Starting server...
Scheduler started
Server listening on 127.0.0.1:9999
Waiting for client connections...
Server is ready. Press Ctrl+C to stop.

Client connected from 127.0.0.1:xxxxx
Request #1 received: 0 /test
Response sent (sendResponse): complete
Connection closed

Client connected from 127.0.0.1:xxxxx
Request #2 received: 0 /api/users?id=123
Response sent (sendHeader+send): complete
Connection closed
```

### 配合客户端测试

需要先启动服务器，然后运行客户端测试：

```bash
# 终端 1：启动服务器
./test/T3-ReaderWriterServer

# 终端 2：运行客户端
./test/T4-ReaderWriterClient
```

## 测试结论

### 功能验证

✅ **HttpReader 功能完善**：正确读取和解析 HTTP 请求
✅ **HttpWriter 多种发送方式**：支持 3 种不同的响应发送方式
✅ **异步 I/O 集成**：与协程和调度器无缝集成
✅ **连接管理正确**：优雅处理连接建立和关闭
✅ **错误处理健壮**：正确处理客户端断开等异常情况

### 性能特点

- **非阻塞 I/O**：使用异步 socket 操作
- **协程驱动**：基于 C++20 协程实现
- **零拷贝读取**：HttpReader 使用 RingBuffer + iovec
- **灵活发送**：支持多种发送方式满足不同需求

### 三种发送方式对比

| 方式 | API | 适用场景 | 优点 |
|------|-----|---------|------|
| sendResponse | 完整响应对象 | 简单响应 | 简洁易用 |
| sendHeader + send | 分离发送 | 流式响应 | 灵活控制 |
| send(buffer) | 原始数据 | 自定义格式 | 完全控制 |

### 适用场景

1. **HTTP 服务器**：标准 HTTP/1.1 服务器实现
2. **API 服务**：RESTful API 后端
3. **文件服务器**：静态文件托管
4. **代理服务器**：HTTP 代理和网关

### 注意事项

1. **RingBuffer 大小**：默认 8192 字节，可根据需求调整
2. **连接复用**：当前实现为短连接，可扩展支持 keep-alive
3. **并发处理**：每个连接在独立协程中处理

---

**测试日期**：2026-01-29
**测试人员**：galay-http 开发团队
**文档版本**：v1.0
