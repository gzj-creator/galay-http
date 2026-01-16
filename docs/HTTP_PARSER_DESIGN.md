# HTTP Parser 增量解析设计

## 概述

HTTP Parser 支持增量解析模式，调用方每次传入新数据，解析器返回已消费的字节数，调用方负责 consume 已解析的数据。

## 核心接口

```cpp
std::pair<HttpErrorCode, ssize_t> fromIOVec(const std::vector<iovec>& iovecs);
```

### 返回值说明

- `{kNoError, consumed}`: 解析成功，`consumed` 为本次消费的字节数
- `{kIncomplete, consumed}`: 数据不完整，`consumed` 为已消费的字节数，需要更多数据
- `{kXxxError, -1}`: 解析错误

## 使用约定

**调用方必须在每次 `fromIOVec` 返回 `consumed > 0` 后调用 `buffer.consume(consumed)`**，确保下次传入的是新数据。

## 使用示例

```cpp
RingBuffer buffer(4096);
HttpRequest request;

// 循环接收数据并解析
while (!request.isComplete()) {
    // 从网络接收数据到 buffer
    receiveData(buffer);

    auto iovecs = buffer.getReadIovecs();
    auto [err, consumed] = request.fromIOVec(iovecs);

    if (err != kNoError && err != kIncomplete) {
        // 解析错误
        handleError(err);
        break;
    }

    // 消费已解析的数据
    if (consumed > 0) {
        buffer.consume(consumed);
    }
}

if (request.isComplete()) {
    // 处理完整请求
    handleRequest(request);
}
```

## 分片解析示例

```cpp
// Header 分多次到达
buffer.write("GET /api HTTP/1.1\r\n");
buffer.write("Host: localhost\r\n");

auto iovecs1 = buffer.getReadIovecs();
auto [err1, consumed1] = request.fromIOVec(iovecs1);
// err1 == kNoError, consumed1 > 0 (header 不完整但已消费)
buffer.consume(consumed1);

buffer.write("\r\n");  // header 结束

auto iovecs2 = buffer.getReadIovecs();
auto [err2, consumed2] = request.fromIOVec(iovecs2);
// err2 == kNoError, consumed2 > 0, request.isComplete() == true
buffer.consume(consumed2);
```

## 错误码说明

| 错误码 | 说明 |
|--------|------|
| `kNoError` | 无错误，解析成功或部分成功 |
| `kIncomplete` | 数据不完整，需要更多数据 |
| `kBadRequest` | 请求格式错误 |
| `kVersionNotSupport` | HTTP 版本不支持 |
| `kHttpCodeInvalid` | HTTP 状态码无效 |

## 设计优势

1. **零拷贝**: 直接从 RingBuffer 的 iovec 解析，无需拷贝数据
2. **内存高效**: 增量消费数据，不需要缓存完整报文
3. **简单可靠**: 调用方只需关注 consume 逻辑，解析器内部状态自动维护
