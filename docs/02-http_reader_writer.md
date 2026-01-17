# HTTP Reader/Writer 模块

## 1. 概述

HttpReader 和 HttpWriter 提供异步的 HTTP 请求读取和响应发送功能，基于协程和零拷贝技术。

## 2. 核心组件

### 2.1 HttpReader
- **文件**: `galay-http/kernel/http/HttpReader.h/cc`
- **功能**: 异步读取和增量解析 HTTP 请求
- **特性**:
  - 自动调用 `socket.readv(ringBuffer.getWriteIovecs())`
  - 完全隐藏 `IOController` 和 `ReadvAwaitable`
  - 返回 `RequestAwaitable`

### 2.2 HttpWriter
- **文件**: `galay-http/kernel/http/HttpWriter.h/cc`
- **功能**: 异步发送 HTTP 响应
- **特性**:
  - 自动将响应序列化为字符串
  - 使用 `socket.send()` 发送数据
  - 完全隐藏 `IOController` 和 `SendAwaitable`
  - 返回 `ResponseAwaitable`
  - 不依赖 `RingBuffer`

### 2.3 HttpReaderSetting
- **文件**: `galay-http/kernel/http/HttpReaderSetting.h`
- **配置项**:
  - `setMaxHeaderSize(size_t)` - 最大头部大小
  - `setMaxBodySize(size_t)` - 最大 Body 大小
  - `setRecvTimeout(int)` - 接收超时时间（毫秒）

### 2.4 HttpWriterSetting
- **文件**: `galay-http/kernel/http/HttpWriterSetting.h`
- **配置项**:
  - `setSendTimeout(int)` - 发送超时时间（毫秒）
  - `setBufferingEnabled(bool)` - 是否启用缓冲
  - `setMaxResponseSize(size_t)` - 最大响应大小

## 3. 架构设计

### 3.1 自动化封装

**HttpReader**:
```cpp
// 构造时接收 TcpSocket& 引用
HttpReader(RingBuffer& ring_buffer, const HttpReaderSetting& setting, TcpSocket& socket);

// getRequest() 内部自动调用 socket.readv()
RequestAwaitable getRequest(HttpRequest& request) {
    return RequestAwaitable(m_ring_buffer, m_setting, request,
                          m_socket.readv(m_ring_buffer.getWriteIovecs()));
}
```

**HttpWriter**:
```cpp
// 构造时接收 TcpSocket& 引用（不需要 RingBuffer）
HttpWriter(const HttpWriterSetting& setting, TcpSocket& socket);

// sendResponse() 内部自动序列化并调用 socket.send()
ResponseAwaitable sendResponse(HttpResponse& response) {
    std::string responseStr = response.toString();
    return ResponseAwaitable(std::move(responseStr),
                            m_socket.send(responseStr.data(), responseStr.size()));
}
```

### 3.2 IOController 隐藏

`IOController` 完全隐藏在内部，通过 `TcpSocket&` 引用访问，外部无需关心。

## 4. 使用示例

### 4.1 基本用法

```cpp
// 创建连接
TcpSocket client(acceptResult.value());
client.option().handleNonBlock();

// 创建 RingBuffer 和配置
RingBuffer ringBuffer(8192);
HttpReaderSetting readerSetting;
HttpWriterSetting writerSetting;

// 创建 Reader 和 Writer
HttpReader reader(ringBuffer, readerSetting, client);
HttpWriter writer(writerSetting, client);  // Writer 不需要 RingBuffer

// 读取请求
HttpRequest request;
auto result = co_await reader.getRequest(request);

if (result && result.value()) {
    // 请求完整，处理业务逻辑
    HttpResponse response;
    // ... 构造响应 ...

    // 发送响应
    auto sendResult = co_await writer.sendResponse(response);
}
```

### 4.2 完整示例

```cpp
Coroutine handleClient(TcpSocket client) {
    RingBuffer ringBuffer(8192);
    HttpReaderSetting readerSetting;
    HttpWriterSetting writerSetting;

    HttpReader reader(ringBuffer, readerSetting, client);
    HttpWriter writer(writerSetting, client);  // Writer 不需要 RingBuffer

    while (true) {
        HttpRequest request;
        auto result = co_await reader.getRequest(request);

        if (!result) {
            // 错误处理
            auto& error = result.error();
            if (error.code() == kConnectionClose) {
                break;
            }
            // 发送错误响应
            HttpResponse errorResponse;
            // ... 构造错误响应 ...
            co_await writer.sendResponse(errorResponse);
            break;
        }

        if (!result.value()) {
            // 数据不完整，继续读取
            continue;
        }

        // 处理请求
        HttpResponse response;
        // ... 业务逻辑 ...

        // 发送响应
        auto sendResult = co_await writer.sendResponse(response);
        if (!sendResult) {
            break;
        }

        // 检查是否需要关闭连接
        if (shouldClose(request)) {
            break;
        }
    }

    co_await client.close();
}
```

## 5. 特性

### 5.1 零拷贝
- 使用 `RingBuffer` 和 `iovec` 避免数据拷贝
- 直接从 socket 读取到 buffer
- 直接从 buffer 发送到 socket

### 5.2 增量解析
- 支持分片数据的增量解析
- 自动处理不完整的请求
- 返回 `std::expected<bool, HttpError>` 表示解析状态

### 5.3 异步 IO
- 基于协程的异步 IO
- 使用 `ReadvAwaitable` 和 `WritevAwaitable`
- 高并发性能

### 5.4 错误处理
- 完善的错误处理机制
- 自动检测连接关闭
- 返回详细的错误信息

## 6. 测试

### 6.1 功能测试

**启动服务器**:
```bash
./test/test_reader_writer_server
```

**运行客户端测试**:
```bash
./test/test_reader_writer_client
```

**手动测试**:
```bash
curl http://127.0.0.1:9999/test
curl http://127.0.0.1:9999/api/users?id=123
curl -X POST -d "test data" http://127.0.0.1:9999/post
```

### 6.2 测试结果

- ✅ 简单 GET 请求
- ✅ 带参数的 GET 请求
- ✅ POST 请求
- ✅ 多个连续请求
- ✅ 并发请求

### 6.3 性能测试

使用 wrk 进行压力测试：

```bash
wrk -t4 -c100 -d10s http://127.0.0.1:9999/test
```

**预期结果**:
- 无错误
- 高吞吐量
- 低延迟

## 7. 验证点

### 7.1 HttpReader 验证
- ✅ 正确解析 HTTP 请求行（方法、路径、版本）
- ✅ 正确解析 HTTP 请求头
- ✅ 增量解析功能正常
- ✅ 使用 RingBuffer 进行零拷贝缓冲
- ✅ 自动调用 `socket.readv()`
- ✅ 完全隐藏 `IOController` 和 `ReadvAwaitable`

### 7.2 HttpWriter 验证
- ✅ 正确序列化 HTTP 响应
- ✅ 异步发送功能正常
- ✅ 使用 SendAwaitable 进行发送
- ✅ 自动管理响应字符串生命周期
- ✅ 自动调用 `socket.send()`
- ✅ 完全隐藏 `IOController` 和 `SendAwaitable`
- ✅ 不依赖 `RingBuffer`

## 8. API 对比

### 8.1 修改前

```cpp
HttpReader reader(ringBuffer, setting);
HttpWriter writer;

// 需要手动获取 iovecs 和创建 awaitable
auto writeIovecs = ringBuffer.getWriteIovecs();
auto readvAwaitable = client.readv(std::move(writeIovecs));
auto result = co_await reader.getRequest(request, std::move(readvAwaitable));

// 需要手动序列化和创建 iovecs
std::string responseStr = response.toString();
std::vector<struct iovec> iovecs(1);
iovecs[0].iov_base = const_cast<char*>(responseStr.data());
iovecs[0].iov_len = responseStr.size();
auto sendResult = co_await writer.sendResponse(response, client.writev(std::move(iovecs)));
```

### 8.2 修改后

```cpp
HttpReader reader(ringBuffer, readerSetting, client);
HttpWriter writer(writerSetting, client);  // Writer 不需要 RingBuffer

// 自动化处理，简洁明了
auto result = co_await reader.getRequest(request);
auto sendResult = co_await writer.sendResponse(response);
```

## 9. 设计优势

1. **更简洁的 API**: 用户不需要手动管理 `ReadvAwaitable` 和 `WritevAwaitable`
2. **更好的封装**: `IOController` 完全隐藏在内部，不暴露给外部
3. **自动化管理**: `RingBuffer` 的 `getWriteIovecs()` 和 `getReadIovecs()` 自动在内部调用
4. **类型安全**: 直接使用 `TcpSocket&` 而不是模板，更明确的类型约束
5. **易于使用**: 减少样板代码，降低使用门槛
