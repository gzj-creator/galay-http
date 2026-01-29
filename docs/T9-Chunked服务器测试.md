# T9-Chunked 服务器测试

## 测试概述

本文档记录 HTTP Chunked 编码服务器端功能的测试结果。测试验证了服务器接收 chunked 请求、解析 chunk 数据、发送 chunked 响应的完整流程。

## 测试目标

验证服务器端 Chunked 编码支持：
- 检测 `Transfer-Encoding: chunked` 头部
- 使用 `HttpReader.getChunk()` 读取 chunk 数据
- 使用 `HttpWriter.sendChunk()` 发送 chunk 响应
- 处理多个 chunk 的接收和发送
- 正确识别最后一个 chunk（`0\r\n\r\n`）

## Chunked 编码说明

### 什么是 Chunked 编码

Chunked 编码是 HTTP/1.1 的一种传输编码方式，用于在不知道内容总长度的情况下传输数据。

**格式**：
```
<chunk-size-in-hex>\r\n
<chunk-data>\r\n
<chunk-size-in-hex>\r\n
<chunk-data>\r\n
...
0\r\n
\r\n
```

**示例**：
```
6\r\n
Hello \r\n
5\r\n
World\r\n
0\r\n
\r\n
```

### 使用场景

1. **动态内容生成**：内容长度未知
2. **流式传输**：边生成边发送
3. **服务器推送**：实时数据推送
4. **大文件传输**：避免一次性加载到内存

## 测试场景

### 1. 服务器启动和监听

#### 1.1 服务器配置
- **监听地址**：127.0.0.1:9999
- **监听队列**：128
- **Socket 选项**：ReuseAddr、NonBlock

#### 1.2 连接接受
- **测试内容**：接受客户端连接
- **验证点**：
  - 连接建立成功
  - 记录客户端信息
  - 创建独立协程处理

### 2. Chunked 请求接收

#### 2.1 检测 Chunked 编码
- **测试内容**：检查请求头 `Transfer-Encoding: chunked`
- **实现**：
  ```cpp
  if (request.header().isChunked()) {
      // 处理 chunked 请求
  }
  ```
- **验证点**：
  - 正确识别 chunked 请求
  - 进入 chunk 读取流程

#### 2.2 读取 Chunk 数据
- **测试内容**：循环读取所有 chunk
- **实现**：
  ```cpp
  std::string allChunkData;
  bool isLast = false;
  int chunkCount = 0;

  while (!isLast) {
      std::string chunkData;
      auto chunkResult = co_await reader.getChunk(chunkData);

      if (!chunkResult) {
          // 错误处理
          break;
      }

      isLast = chunkResult.value();

      if (!chunkData.empty()) {
          chunkCount++;
          allChunkData += chunkData;
      }
  }
  ```
- **验证点**：
  - 每个 chunk 正确读取
  - chunk 数据正确拼接
  - 正确识别最后一个 chunk
  - chunk 计数正确

#### 2.3 Chunk 数据统计
- **统计信息**：
  - chunk 数量
  - 总字节数
  - 每个 chunk 的大小
- **日志输出**：
  ```
  Received chunk #1: 6 bytes
  Received chunk #2: 5 bytes
  Received chunk #3: 8 bytes
  All chunks received. Total: 3 chunks, 19 bytes
  ```

### 3. Chunked 响应发送

#### 3.1 发送响应头
- **测试内容**：发送带 `Transfer-Encoding: chunked` 的响应头
- **响应头**：
  ```cpp
  HttpResponseHeader respHeader;
  respHeader.version() = HttpVersion::HttpVersion_1_1;
  respHeader.code() = HttpStatusCode::OK_200;
  respHeader.headerPairs().addHeaderPair("Content-Type", "text/plain");
  respHeader.headerPairs().addHeaderPair("Transfer-Encoding", "chunked");
  respHeader.headerPairs().addHeaderPair("Server", "galay-http-chunked-test/1.0");
  ```
- **验证点**：
  - 响应头正确构造
  - `Transfer-Encoding: chunked` 存在
  - 无 `Content-Length` 头部

#### 3.2 发送多个 Chunk
- **测试内容**：发送 3 个数据 chunk
- **Chunk 内容**：
  1. `"Received X chunks\n"`
  2. `"Total bytes: Y\n"`
  3. `"Echo: <原始数据>"`
- **实现**：
  ```cpp
  // Chunk 1
  std::string chunk1 = "Received " + std::to_string(chunkCount) + " chunks\n";
  auto chunk1Result = co_await writer.sendChunk(chunk1, false);

  // Chunk 2
  std::string chunk2 = "Total bytes: " + std::to_string(allChunkData.size()) + "\n";
  auto chunk2Result = co_await writer.sendChunk(chunk2, false);

  // Chunk 3
  std::string chunk3 = "Echo: " + allChunkData;
  auto chunk3Result = co_await writer.sendChunk(chunk3, false);

  // 最后一个 chunk
  std::string emptyChunk;
  auto lastChunkResult = co_await writer.sendChunk(emptyChunk, true);
  ```
- **验证点**：
  - 每个 chunk 发送成功
  - 返回发送字节数
  - 最后一个 chunk 正确标记

#### 3.3 结束标记
- **测试内容**：发送 `0\r\n\r\n` 表示传输结束
- **实现**：`sendChunk("", true)`
- **验证点**：
  - 结束标记发送成功
  - 客户端能够识别结束

### 4. 非 Chunked 请求处理

#### 4.1 普通请求
- **测试内容**：处理非 chunked 的普通请求
- **响应**：
  - 状态码：200 OK
  - Content-Length：正确计算
  - Body：`"Non-chunked request received\n"`
- **验证点**：
  - 正确区分 chunked 和非 chunked
  - 普通请求正常处理

### 5. 错误处理

#### 5.1 Chunk 解析错误
- **测试内容**：处理 chunk 格式错误
- **验证点**：
  - 检测到解析错误
  - 记录错误日志
  - 关闭连接

#### 5.2 连接断开
- **测试内容**：处理客户端断开连接
- **验证点**：
  - 检测到 `kConnectionClose`
  - 优雅处理断开

## 测试用例列表

| 编号 | 测试场景 | 输入 | 预期输出 | 结果 |
|------|---------|------|---------|------|
| 1 | 服务器启动 | - | ✓ 监听成功 | ✓ |
| 2 | 检测 chunked | Transfer-Encoding: chunked | ✓ 识别成功 | ✓ |
| 3 | 读取 chunk | 多个 chunk | ✓ 全部读取 | ✓ |
| 4 | 识别结束 | 0\r\n\r\n | ✓ isLast=true | ✓ |
| 5 | 发送 chunk | 3 个 chunk | ✓ 全部发送 | ✓ |
| 6 | 发送结束 | 空 chunk | ✓ 结束标记 | ✓ |
| 7 | 非 chunked | 普通请求 | ✓ 正常处理 | ✓ |

## 测试代码位置

- **文件路径**：`/Users/gongzhijie/Desktop/projects/git/galay-http/test/T9-ChunkedServer.cc`
- **代码行数**：431 行
- **主要函数**：
  - `chunkedTestServer()`：服务器主协程
  - 支持 Kqueue、Epoll、IOUring

## 核心 API 说明

### HttpReader Chunk API

```cpp
// 读取一个 chunk
Awaitable<std::expected<bool, HttpError>> getChunk(std::string& chunkData);
```

**返回值**：
- `true`：这是最后一个 chunk
- `false`：还有更多 chunk
- `error`：解析错误

**使用模式**：
```cpp
bool isLast = false;
while (!isLast) {
    std::string chunkData;
    auto result = co_await reader.getChunk(chunkData);

    if (!result) {
        // 错误处理
        break;
    }

    isLast = result.value();
    // 处理 chunkData
}
```

### HttpWriter Chunk API

```cpp
// 发送一个 chunk
Awaitable<std::expected<ssize_t, HttpError>>
    sendChunk(const std::string& data, bool isLast);
```

**参数**：
- `data`：chunk 数据（可以为空）
- `isLast`：是否为最后一个 chunk

**使用模式**：
```cpp
// 发送数据 chunk
co_await writer.sendChunk("Hello", false);
co_await writer.sendChunk("World", false);

// 发送结束 chunk
co_await writer.sendChunk("", true);
```

## 运行测试

### 编译测试

```bash
cd build
cmake ..
make T9-ChunkedServer
```

### 运行服务器

```bash
./test/T9-ChunkedServer
```

### 预期输出

```
========================================
HTTP Chunked Encoding Test - Server
========================================

Scheduler started
=== HTTP Chunked Encoding Test Server ===
Starting server...
Server listening on 127.0.0.1:9999
Waiting for client connections...
Server is ready. Press Ctrl+C to stop.

Client connected from 127.0.0.1:xxxxx
Request #1 received: 1 /test
Detected chunked transfer encoding
Received chunk #1: 6 bytes
Received chunk #2: 5 bytes
Received chunk #3: 8 bytes
Received chunk #4: 7 bytes
All chunks received. Total: 4 chunks, 26 bytes
Chunk data: Hello from chunked client!
Chunked response sent successfully
Connection closed
```

### 配合客户端测试

```bash
# 终端 1：启动服务器
./test/T9-ChunkedServer

# 终端 2：运行客户端
./test/T10-ChunkedClient
```

## 测试结论

### 功能验证

✅ **Chunked 检测正确**：准确识别 `Transfer-Encoding: chunked`
✅ **Chunk 读取完善**：正确读取所有 chunk 数据
✅ **结束标记识别**：准确识别最后一个 chunk
✅ **Chunk 发送正常**：成功发送多个 chunk
✅ **数据完整性**：chunk 数据无损传输
✅ **错误处理健壮**：正确处理各种错误情况

### Chunked 编码优势

| 特性 | Chunked | Content-Length |
|------|---------|----------------|
| 提前知道长度 | ❌ 不需要 | ✅ 必需 |
| 流式传输 | ✅ 支持 | ❌ 需缓存 |
| 内存占用 | 低 | 高（需缓存） |
| 实时性 | 高 | 低 |
| 额外开销 | ~10 字节/chunk | 无 |

### 性能特点

- **流式处理**：边接收边处理，无需等待完整数据
- **内存高效**：不需要缓存完整请求/响应
- **低延迟**：数据到达即可处理
- **适合大文件**：避免一次性加载到内存

### 适用场景

1. **文件上传**：大文件分块上传
2. **流式 API**：实时数据推送
3. **服务器推送**：SSE（Server-Sent Events）
4. **动态内容**：内容长度未知的响应
5. **视频流**：流媒体传输

### 实现细节

**Chunk 大小建议**：
- 小 chunk：1KB - 8KB（低延迟）
- 中等 chunk：8KB - 64KB（平衡）
- 大 chunk：64KB - 1MB（高吞吐）

**注意事项**：
1. **结束标记必需**：必须发送 `0\r\n\r\n`
2. **不能混用**：不能同时使用 `Transfer-Encoding` 和 `Content-Length`
3. **HTTP/1.1 特性**：HTTP/1.0 不支持
4. **代理兼容性**：某些老旧代理可能不支持

### 与其他传输方式对比

| 方式 | 适用场景 | 优点 | 缺点 |
|------|---------|------|------|
| Content-Length | 静态内容 | 简单、高效 | 需要提前知道长度 |
| Chunked | 动态内容 | 流式、实时 | 额外开销 |
| Multipart | 文件上传 | 支持多文件 | 复杂 |
| WebSocket | 双向通信 | 全双工 | 需要升级协议 |

---

**测试日期**：2026-01-29
**测试人员**：galay-http 开发团队
**文档版本**：v1.0
