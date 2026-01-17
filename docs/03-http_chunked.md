# HTTP Chunked 编码支持

## 概述

galay-http 完整支持 HTTP Chunked Transfer Encoding (RFC 7230)，提供高性能的 chunk 编码和解析功能。

## 核心组件

### 1. HttpChunk 类

**位置**: `galay-http/protoc/http/HttpChunk.h/cc`

提供 chunk 编码和解析的静态方法：

```cpp
class Chunk {
public:
    // 从 iovec 解析 chunk 数据
    static std::expected<std::pair<bool, size_t>, HttpError>
    fromIOVec(const std::vector<iovec>& iovecs, std::string& chunk_data);

    // 创建 chunk 编码数据
    static std::string toChunk(const std::string& data, bool is_last = false);
    static std::string toChunk(const char* data, size_t length, bool is_last = false);
};
```

### 2. HttpReader 增强

**新增接口**:
- `GetChunkAwaitable getChunk(std::string& chunk_data)`: 异步读取 chunk 数据

**特性**:
- 支持增量读取 chunk 数据
- 自动处理 RingBuffer 消费
- 追加方式写入用户缓冲区
- 正确检测最后一个 chunk (size=0)

### 3. HttpWriter 优化

**新增接口**:
- `SendResponseAwaitable sendChunk(const std::string& data, bool is_last = false)`: 发送 chunk 数据

**断点续传支持**:
- 内部维护 `m_buffer` 和 `m_remaining_bytes`
- 如果一次 send 没发完，第二次会从上次位置继续
- 发送完成后自动清空 buffer 和重置状态

## 使用示例

### 服务器端接收 Chunked 请求

```cpp
// 1. 读取请求头
HttpRequest request;
auto result = co_await reader.getRequest(request);

// 2. 检查是否是 chunked 编码
if (result.value() == false && request.header().isChunked()) {
    std::string allData;
    bool isLast = false;

    // 3. 循环读取所有 chunk
    while (!isLast) {
        auto chunkResult = co_await reader.getChunk(allData);
        if (!chunkResult) {
            // 错误处理
            break;
        }
        isLast = chunkResult.value();
        // allData 包含所有已接收的 chunk 数据（追加方式）
    }

    LogInfo("Received all chunks: {} bytes", allData.size());
}
```

### 服务器端发送 Chunked 响应

```cpp
// 1. 发送响应头
HttpResponseHeader header;
header.version() = HttpVersion::HttpVersion_1_1;
header.code() = HttpStatusCode::OK_200;
header.headerPairs().addHeaderPair("Transfer-Encoding", "chunked");
co_await writer.sendHeader(std::move(header));

// 2. 发送多个 chunk
co_await writer.sendChunk("First chunk\n", false);
co_await writer.sendChunk("Second chunk\n", false);
co_await writer.sendChunk("Third chunk\n", false);

// 3. 发送最后一个 chunk
std::string empty;
co_await writer.sendChunk(empty, true);
```

### 客户端发送 Chunked 请求

```cpp
// 1. 发送请求头
HttpRequestHeader header;
header.method() = HttpMethod::HttpMethod_Post;
header.uri() = "/upload";
header.headerPairs().addHeaderPair("Transfer-Encoding", "chunked");
co_await writer.sendHeader(std::move(header));

// 2. 发送数据块
co_await writer.sendChunk("chunk1", false);
co_await writer.sendChunk("chunk2", false);

// 3. 发送最后一个 chunk
std::string empty;
co_await writer.sendChunk(empty, true);
```

## 性能数据

基于 benchmark/bench_chunked.cc 的测试结果：

| 操作 | 吞吐量 | 平均延迟 |
|------|--------|----------|
| Chunk 编码 | 400万 ops/sec | 0.25 μs/op |
| Chunk 解析 | 217万 ops/sec | 0.46 μs/op |
| RingBuffer 集成 | 96万 ops/sec | 1.04 μs/op |

## 核心特性

### 1. 断点续传
如果一次 send 没发完，第二次调用会从上次位置继续发送，不会重新生成字符串。

```cpp
// Writer 内部实现
if (m_remaining_bytes == 0) {
    m_buffer = Chunk::toChunk(data, is_last);
    m_remaining_bytes = m_buffer.size();
}

// 计算当前发送位置
size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
const char* send_ptr = m_buffer.data() + sent_bytes;
```

### 2. 零拷贝
- buffer 只生成一次，避免重复序列化
- 直接从 RingBuffer 读取数据进行解析
- 追加方式写入用户缓冲区

### 3. 增量解析
- 支持分片接收 chunk 数据
- 正确检测数据不完整
- 自动处理跨 iovec 的 chunk

### 4. 自动管理
- 发送完成后自动清空 buffer
- 自动更新剩余发送字节数
- 自动消费 RingBuffer

## 测试覆盖

### 单元测试 (test/test_chunk.cc)
- ✅ 普通 chunk 编码/解析
- ✅ 最后一个 chunk (size=0)
- ✅ 多个 chunk 连续解析
- ✅ 数据不完整检测
- ✅ 跨 iovec 的 chunk 解析
- ✅ 编码/解码往返测试

### 集成测试 (test/test_chunk_integration.cc)
- ✅ 与 RingBuffer 集成
- ✅ 分片接收测试
- ✅ 完整的编码/解码流程

### 端到端测试
- ✅ test_chunked_server.cc: Chunked 服务器
- ✅ test_chunked_client.cc: Chunked 客户端
- ✅ 客户端-服务器通信测试

## Chunk 编码格式

HTTP Chunked 编码格式 (RFC 7230):

```
chunk-size(hex)\r\n
chunk-data\r\n
chunk-size(hex)\r\n
chunk-data\r\n
...
0\r\n
\r\n
```

示例：
```
5\r\n
Hello\r\n
6\r\n
World!\r\n
0\r\n
\r\n
```

## API 参考

### HttpReader::getChunk()

```cpp
GetChunkAwaitable getChunk(std::string& chunk_data);
```

**参数**:
- `chunk_data`: 用户传入的 string 引用，用于接收 chunk 数据（追加方式）

**返回值**: `std::expected<bool, HttpError>`
- `true`: 读取到最后一个 chunk (size=0)，所有 chunk 读取完成
- `false`: 读取到 chunk 数据但不是最后一个，需要继续调用
- `HttpError`: 解析错误

### HttpWriter::sendChunk()

```cpp
SendResponseAwaitable sendChunk(const std::string& data, bool is_last = false);
```

**参数**:
- `data`: 要发送的数据
- `is_last`: 是否是最后一个 chunk

**返回值**: `std::expected<size_t, HttpError>`
- `size_t`: 发送的字节数
- `HttpError`: 发送错误

### Chunk::fromIOVec()

```cpp
static std::expected<std::pair<bool, size_t>, HttpError>
fromIOVec(const std::vector<iovec>& iovecs, std::string& chunk_data);
```

**参数**:
- `iovecs`: 输入的 iovec 数组
- `chunk_data`: 输出的 chunk 数据（追加方式）

**返回值**: `std::expected<std::pair<bool, size_t>, HttpError>`
- `pair.first`: true 表示读取到最后一个 chunk
- `pair.second`: 消费的字节数
- `HttpError`: 解析错误或数据不完整 (kIncomplete)

### Chunk::toChunk()

```cpp
static std::string toChunk(const std::string& data, bool is_last = false);
static std::string toChunk(const char* data, size_t length, bool is_last = false);
```

**参数**:
- `data`: 要编码的数据
- `is_last`: 是否是最后一个 chunk

**返回值**: chunk 编码后的字符串

## 错误处理

常见错误码：
- `kIncomplete`: 数据不完整，需要继续接收
- `kInvalidChunkFormat`: Chunk 格式错误
- `kChunkSizeConvertError`: Chunk 大小转换错误
- `kConnectionClose`: 连接关闭
- `kRecvError`: 接收错误
- `kSendError`: 发送错误

## 注意事项

1. **getRequest() 返回值**: 当 header 解析完成且是 chunked 编码时，`getRequest()` 返回 `false`，用户需要检查 `isChunked()` 并调用 `getChunk()` 继续读取

2. **追加模式**: chunk 数据以追加方式写入用户缓冲区，不会清空之前的数据

3. **断点续传**: Writer 会自动处理断点续传，用户无需关心发送状态

4. **最后一个 chunk**: 发送最后一个 chunk 时，data 应该为空字符串，is_last 为 true

## 示例程序

- `test/test_chunk.cc`: Chunk 类单元测试
- `test/test_chunk_integration.cc`: Chunk 集成测试
- `test/test_chunked_server.cc`: Chunked 服务器示例
- `test/test_chunked_client.cc`: Chunked 客户端示例
- `benchmark/bench_chunked.cc`: 性能测试

运行测试：
```bash
# 单元测试
./test/test_chunk

# 集成测试
./test/test_chunk_integration

# 服务器（在一个终端）
./test/test_chunked_server

# 客户端（在另一个终端）
./test/test_chunked_client

# 性能测试
./benchmark/bench_chunked
```
