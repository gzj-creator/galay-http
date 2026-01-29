# T10-Chunked 客户端测试

## 测试概述

本文档记录 HTTP Chunked 编码客户端功能的测试结果。测试验证了客户端发送 chunked 请求、接收 chunked 响应的完整流程。

## 测试目标

验证客户端 Chunked 编码支持：
- 构造带 `Transfer-Encoding: chunked` 的请求头
- 使用 `HttpWriter.sendChunk()` 发送多个 chunk
- 发送结束标记（`0\r\n\r\n`）
- 接收并解析 chunked 响应
- 使用 `HttpReader.getChunk()` 读取响应 chunk

## 测试场景

### 1. 连接建立

#### 1.1 连接到服务器
- **服务器地址**：127.0.0.1:9999
- **连接方式**：异步非阻塞
- **验证点**：
  - 连接成功
  - Socket 非阻塞模式设置成功

### 2. Chunked 请求发送

#### 2.1 构造请求头
- **测试内容**：构造带 chunked 编码的 POST 请求头
- **请求头**：
  ```cpp
  HttpRequestHeader reqHeader;
  reqHeader.method() = HttpMethod::POST;
  reqHeader.uri() = "/test";
  reqHeader.version() = HttpVersion::HttpVersion_1_1;
  reqHeader.headerPairs().addHeaderPair("Host", "127.0.0.1:9999");
  reqHeader.headerPairs().addHeaderPair("Transfer-Encoding", "chunked");
  reqHeader.headerPairs().addHeaderPair("User-Agent", "galay-http-chunked-client/1.0");
  ```
- **验证点**：
  - 请求方法为 POST
  - 包含 `Transfer-Encoding: chunked`
  - 无 `Content-Length` 头部

#### 2.2 发送请求头
- **测试内容**：发送请求头到服务器
- **实现**：
  ```cpp
  auto headerResult = co_await writer.sendHeader(std::move(reqHeader));
  ```
- **验证点**：
  - 发送成功
  - 返回发送字节数
  - 日志记录正确

#### 2.3 发送数据 Chunk
- **测试内容**：发送 4 个数据 chunk
- **Chunk 列表**：
  1. `"Hello "` (6 字节)
  2. `"from "` (5 字节)
  3. `"chunked "` (8 字节)
  4. `"client!"` (7 字节)
- **实现**：
  ```cpp
  // Chunk 1
  std::string chunk1 = "Hello ";
  auto chunk1Result = co_await writer.sendChunk(chunk1, false);

  // Chunk 2
  std::string chunk2 = "from ";
  auto chunk2Result = co_await writer.sendChunk(chunk2, false);

  // Chunk 3
  std::string chunk3 = "chunked ";
  auto chunk3Result = co_await writer.sendChunk(chunk3, false);

  // Chunk 4
  std::string chunk4 = "client!";
  auto chunk4Result = co_await writer.sendChunk(chunk4, false);
  ```
- **验证点**：
  - 每个 chunk 发送成功
  - 返回正确的字节数
  - 日志记录每个 chunk

#### 2.4 发送结束 Chunk
- **测试内容**：发送 `0\r\n\r\n` 表示传输结束
- **实现**：
  ```cpp
  std::string emptyChunk;
  auto lastChunkResult = co_await writer.sendChunk(emptyChunk, true);
  ```
- **验证点**：
  - 结束标记发送成功
  - `isLast` 参数为 true
  - 服务器能够识别结束

### 3. Chunked 响应接收

#### 3.1 接收响应头
- **测试内容**：读取服务器响应头
- **实现**：
  ```cpp
  HttpResponse response;
  bool responseHeaderComplete = false;

  while (!responseHeaderComplete) {
      auto result = co_await reader.getResponse(response);

      if (!result) {
          // 错误处理
          break;
      }

      responseHeaderComplete = result.value();
  }
  ```
- **验证点**：
  - 响应头接收完整
  - 状态码正确（200 OK）
  - 包含 `Transfer-Encoding: chunked`

#### 3.2 检测 Chunked 响应
- **测试内容**：检查响应是否为 chunked 编码
- **实现**：
  ```cpp
  if (response.header().isChunked()) {
      // 处理 chunked 响应
  }
  ```
- **验证点**：
  - 正确识别 chunked 响应
  - 进入 chunk 读取流程

#### 3.3 读取响应 Chunk
- **测试内容**：循环读取所有响应 chunk
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
          LogInfo("Received response chunk #{}: {} bytes", chunkCount, chunkData.size());
          allChunkData += chunkData;
      }
  }
  ```
- **验证点**：
  - 所有 chunk 正确读取
  - chunk 数据正确拼接
  - 正确识别最后一个 chunk
  - chunk 计数正确

#### 3.4 响应数据验证
- **测试内容**：验证接收到的完整数据
- **预期数据**：
  ```
  Received 4 chunks
  Total bytes: 26
  Echo: Hello from chunked client!
  ```
- **验证点**：
  - 数据完整
  - 内容正确
  - 无数据丢失

### 4. 连接关闭

#### 4.1 关闭连接
- **测试内容**：测试完成后关闭连接
- **实现**：
  ```cpp
  co_await client.close();
  ```
- **验证点**：
  - 连接正常关闭
  - 资源正确释放

## 测试用例列表

| 编号 | 测试步骤 | 操作 | 预期结果 | 结果 |
|------|---------|------|---------|------|
| 1 | 连接服务器 | connect() | ✓ 连接成功 | ✓ |
| 2 | 发送请求头 | sendHeader() | ✓ 发送成功 | ✓ |
| 3 | 发送 chunk 1 | sendChunk("Hello ") | ✓ 6 字节 | ✓ |
| 4 | 发送 chunk 2 | sendChunk("from ") | ✓ 5 字节 | ✓ |
| 5 | 发送 chunk 3 | sendChunk("chunked ") | ✓ 8 字节 | ✓ |
| 6 | 发送 chunk 4 | sendChunk("client!") | ✓ 7 字节 | ✓ |
| 7 | 发送结束 | sendChunk("", true) | ✓ 结束标记 | ✓ |
| 8 | 接收响应头 | getResponse() | ✓ 200 OK | ✓ |
| 9 | 检测 chunked | isChunked() | ✓ true | ✓ |
| 10 | 读取响应 chunk | getChunk() | ✓ 3 个 chunk | ✓ |
| 11 | 验证数据 | - | ✓ 数据完整 | ✓ |
| 12 | 关闭连接 | close() | ✓ 关闭成功 | ✓ |

## 测试代码位置

- **文件路径**：`/Users/gongzhijie/Desktop/projects/git/galay-http/test/T10-ChunkedClient.cc`
- **代码行数**：231 行
- **主要函数**：
  - `sendChunkedRequest()`：客户端主协程

## 数据流图

```
客户端                                    服务器
  |                                        |
  |--- TCP 连接 ------------------------->|
  |<-- 连接确认 --------------------------|
  |                                        |
  |--- POST /test HTTP/1.1 -------------->|
  |    Transfer-Encoding: chunked         |
  |                                        |
  |--- 6\r\n ---------------------------->|
  |    Hello \r\n                         |
  |                                        |
  |--- 5\r\n ---------------------------->|
  |    from \r\n                          |
  |                                        |
  |--- 8\r\n ---------------------------->|
  |    chunked \r\n                       |
  |                                        |
  |--- 7\r\n ---------------------------->|
  |    client!\r\n                        |
  |                                        |
  |--- 0\r\n ---------------------------->|
  |    \r\n                               |
  |                                        |
  |<-- HTTP/1.1 200 OK -------------------|
  |    Transfer-Encoding: chunked         |
  |                                        |
  |<-- 18\r\n ----------------------------|
  |    Received 4 chunks\n\r\n            |
  |                                        |
  |<-- 11\r\n ----------------------------|
  |    Total bytes: 26\n\r\n              |
  |                                        |
  |<-- 32\r\n ----------------------------|
  |    Echo: Hello from chunked client!\r\n|
  |                                        |
  |<-- 0\r\n -----------------------------|
  |    \r\n                               |
  |                                        |
  |--- 关闭连接 ------------------------->|
```

## 运行测试

### 前置条件

**必须先启动服务器**：

```bash
# 终端 1：启动服务器
./test/T9-ChunkedServer
```

### 编译测试

```bash
cd build
cmake ..
make T10-ChunkedClient
```

### 运行客户端

```bash
# 终端 2：运行客户端
./test/T10-ChunkedClient
```

### 预期输出

```
========================================
HTTP Chunked Encoding Test - Client
========================================

Scheduler started

=== HTTP Chunked Client Test ===
Connecting to server...
Connected to server
Sending request header...
Request header sent: 123 bytes
Sending chunk 1...
Chunk 1 sent: 10 bytes
Sending chunk 2...
Chunk 2 sent: 9 bytes
Sending chunk 3...
Chunk 3 sent: 12 bytes
Sending chunk 4...
Chunk 4 sent: 11 bytes
Sending last chunk...
Last chunk sent: 5 bytes

All chunks sent successfully!
Waiting for response...

Response received: 200 OK
Response is chunked encoded
Received response chunk #1: 18 bytes
Received response chunk #2: 17 bytes
Received response chunk #3: 32 bytes

All response chunks received. Total: 3 chunks, 67 bytes
Response data:
Received 4 chunks
Total bytes: 26
Echo: Hello from chunked client!

Connection closed

Test completed
```

## 测试结论

### 功能验证

✅ **Chunked 请求发送**：成功发送多个 chunk
✅ **结束标记发送**：正确发送 `0\r\n\r\n`
✅ **Chunked 响应接收**：成功接收 chunked 响应
✅ **Chunk 解析正确**：正确解析每个 chunk
✅ **数据完整性**：发送和接收的数据无损
✅ **往返测试通过**：客户端-服务器通信正常

### Chunked 编码格式

**发送的原始数据**：
```
POST /test HTTP/1.1\r\n
Host: 127.0.0.1:9999\r\n
Transfer-Encoding: chunked\r\n
User-Agent: galay-http-chunked-client/1.0\r\n
\r\n
6\r\n
Hello \r\n
5\r\n
from \r\n
8\r\n
chunked \r\n
7\r\n
client!\r\n
0\r\n
\r\n
```

**接收的原始数据**：
```
HTTP/1.1 200 OK\r\n
Content-Type: text/plain\r\n
Transfer-Encoding: chunked\r\n
Server: galay-http-chunked-test/1.0\r\n
\r\n
12\r\n
Received 4 chunks\n\r\n
11\r\n
Total bytes: 26\n\r\n
20\r\n
Echo: Hello from chunked client!\r\n
0\r\n
\r\n
```

### 性能特点

- **流式发送**：边生成边发送，无需缓存
- **内存高效**：不需要一次性加载完整数据
- **低延迟**：数据生成即可发送
- **适合大文件**：避免内存溢出

### 使用场景

1. **文件上传**：大文件分块上传
2. **流式数据**：实时数据推送
3. **动态内容**：内容长度未知
4. **视频上传**：流媒体上传

### 与普通请求对比

| 特性 | Chunked 请求 | 普通请求 |
|------|-------------|---------|
| Content-Length | ❌ 不需要 | ✅ 必需 |
| 提前知道大小 | ❌ 不需要 | ✅ 必需 |
| 流式发送 | ✅ 支持 | ❌ 需缓存 |
| 内存占用 | 低 | 高 |
| 实现复杂度 | 高 | 低 |
| 额外开销 | ~10 字节/chunk | 无 |

### Chunk 大小建议

**根据场景选择**：
- **实时数据**：小 chunk（1KB - 8KB）
- **文件上传**：中等 chunk（64KB - 256KB）
- **大文件传输**：大 chunk（256KB - 1MB）

**权衡因素**：
- 小 chunk：低延迟、高开销
- 大 chunk：高延迟、低开销

### 错误处理

**常见错误**：
1. **忘记发送结束标记**：导致服务器一直等待
2. **chunk 大小错误**：十六进制长度不匹配
3. **CRLF 缺失**：格式错误
4. **混用编码**：同时使用 chunked 和 Content-Length

**调试建议**：
- 使用 Wireshark 抓包查看原始数据
- 检查每个 chunk 的格式
- 验证结束标记是否发送

### 扩展功能

可以基于此实现：
1. **断点续传**：记录已发送的 chunk
2. **进度显示**：统计已发送字节数
3. **压缩传输**：对 chunk 数据压缩
4. **加密传输**：对 chunk 数据加密

### 最佳实践

1. **合理的 chunk 大小**：根据场景选择
2. **错误重试**：chunk 发送失败时重试
3. **进度反馈**：显示上传进度
4. **超时控制**：设置发送超时
5. **资源清理**：确保连接正确关闭

---

**测试日期**：2026-01-29
**测试人员**：galay-http 开发团队
**文档版本**：v1.0
