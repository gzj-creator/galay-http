# Timeout Support for All Awaitables

## 概述

galay-http 现在为所有 Awaitable 类型提供了完整的超时支持。所有异步操作都可以通过 `.timeout()` 方法设置超时时间。

## 支持的 Awaitable 类型

### 1. TcpSocket Awaitables (galay-kernel)
- `ConnectAwaitable` - 连接超时
- `RecvAwaitable` - 接收超时
- `SendAwaitable` - 发送超时
- `ReadvAwaitable` - 批量接收超时
- `WritevAwaitable` - 批量发送超时
- `CloseAwaitable` - 关闭超时
- `AcceptAwaitable` - 接受连接超时

### 2. HTTP Awaitables (galay-http)
- `HttpClientAwaitable` - HTTP 客户端请求超时
- `GetRequestAwaitable` - 读取 HTTP 请求超时
- `GetResponseAwaitable` - 读取 HTTP 响应超时
- `GetChunkAwaitable` - 读取 Chunk 数据超时
- `SendResponseAwaitable` - 发送 HTTP 响应超时

### 3. WebSocket Awaitables (galay-http)
- `GetFrameAwaitable` - 读取 WebSocket 帧超时
- `GetMessageAwaitable` - 读取 WebSocket 消息超时
- `SendFrameAwaitable` - 发送 WebSocket 帧超时

## 使用方法

### 基本用法

```cpp
using namespace std::chrono_literals;

// 设置 5 秒超时
auto result = co_await socket.connect(host).timeout(5000ms);

// 或使用 std::chrono::seconds
auto result = co_await socket.connect(host).timeout(std::chrono::seconds(5));
```

### HTTP 客户端超时

```cpp
// HttpClientAwaitable 超时
HttpClient client(std::move(socket));

// GET 请求，5 秒超时
auto result = co_await client.get("/api/data").timeout(5000ms);

if (!result) {
    if (result.error().code() == kRequestTimeOut) {
        // 处理超时
        std::cout << "Request timed out!" << std::endl;
    }
}
```

### HTTP Reader/Writer 超时

```cpp
HttpReader reader(ring_buffer, setting, socket);
HttpWriter writer(setting, socket);

// 发送请求，3 秒超时
auto send_result = co_await writer.sendRequest(request).timeout(3000ms);

// 接收响应，5 秒超时
HttpResponse response;
auto recv_result = co_await reader.getResponse(response).timeout(5000ms);
```

### WebSocket 超时

```cpp
WsReader reader(ring_buffer, setting, socket);
WsWriter writer(setting, socket);

// 读取帧，10 秒超时
WsFrame frame;
auto result = co_await reader.getFrame(frame).timeout(10000ms);

// 发送消息，5 秒超时
auto send_result = co_await writer.sendText("Hello").timeout(5000ms);
```

### TcpSocket 超时

```cpp
TcpSocket socket(IPType::IPV4);

// 连接超时（2 秒）
auto connect_result = co_await socket.connect(host).timeout(2000ms);

// 接收超时（5 秒）
char buffer[1024];
auto recv_result = co_await socket.recv(buffer, sizeof(buffer)).timeout(5000ms);

// 发送超时（3 秒）
auto send_result = co_await socket.send(data, len).timeout(3000ms);
```

## 错误处理

超时会返回错误，错误码为 `kTimeout` (IOError) 或 `kRequestTimeOut` / `kRecvTimeOut` (HttpError)：

```cpp
auto result = co_await client.get("/api").timeout(5000ms);

if (!result) {
    auto& error = result.error();

    if (error.code() == kRequestTimeOut || error.code() == kRecvTimeOut) {
        std::cout << "Operation timed out: " << error.message() << std::endl;
    } else {
        std::cout << "Other error: " << error.message() << std::endl;
    }
}
```

## 实现原理

### TimeoutSupport CRTP 基类

所有 Awaitable 都继承自 `TimeoutSupport<Derived>`：

```cpp
class HttpClientAwaitable : public galay::kernel::TimeoutSupport<HttpClientAwaitable>
{
    // ...
public:
    // TimeoutSupport 需要访问此成员来设置超时错误
    std::expected<std::optional<HttpResponse>, galay::kernel::IOError> m_result;
};
```

### WithTimeout 包装器

`.timeout()` 方法返回一个 `WithTimeout<Awaitable>` 包装器：

```cpp
template<typename Awaitable>
struct WithTimeout {
    Awaitable m_inner;
    TimeoutTimer::ptr m_timer;

    auto await_resume() {
        // 检查是否超时
        if (m_timer->timeouted()) {
            m_inner.m_result = std::unexpected(IOError(kTimeout, 0));
        } else {
            m_timer->cancel();
        }
        return m_inner.await_resume();
    }
};
```

### 错误转换

HTTP 层的 Awaitable 会将 `IOError` 转换为 `HttpError`：

```cpp
std::expected<bool, HttpError> await_resume() {
    // 检查超时错误
    if (!m_result.has_value()) {
        auto& io_error = m_result.error();

        HttpErrorCode http_error_code;
        if (io_error.code() == kTimeout) {
            http_error_code = kRequestTimeOut;
        }

        return std::unexpected(HttpError(http_error_code, io_error.message()));
    }

    // 正常处理...
}
```

## 测试验证

### 连接超时测试

```bash
$ ./test_socket_timeout
✓ ConnectAwaitable.timeout() works! (1961ms)  # 设置 2000ms
✓ RecvAwaitable.timeout() works! (964ms)      # 设置 1000ms
```

### HTTP 超时测试

```bash
$ ./test_all_awaitable_timeout_complete
✓ HttpClientAwaitable.timeout() works!
✓ SendResponseAwaitable.timeout() works!
✓ GetResponseAwaitable.timeout() works!
```

## 性能特性

- **精确度高**：超时误差通常在 50ms 以内（< 2.5%）
- **零开销**：不使用 `.timeout()` 时没有额外开销
- **可组合**：可以与其他异步操作组合使用

## 注意事项

1. **超时时间单位**：使用 `std::chrono::milliseconds` 或其他 `std::chrono` 时间单位
2. **错误码检查**：超时错误码为 `kTimeout` (IOError) 或 `kRequestTimeOut`/`kRecvTimeOut` (HttpError)
3. **资源清理**：超时后会自动清理相关资源
4. **重试机制**：超时后可以重新发起请求

## 示例：完整的超时处理

```cpp
Coroutine handleRequest(IOScheduler* scheduler) {
    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    // 连接超时：5 秒
    Host host(IPType::IPV4, "example.com", 80);
    auto connect_result = co_await socket.connect(host).timeout(5000ms);

    if (!connect_result) {
        if (connect_result.error().code() == kTimeout) {
            std::cout << "Connection timeout, retrying..." << std::endl;
            // 重试逻辑
        }
        co_return;
    }

    HttpClient client(std::move(socket));

    // 请求超时：10 秒
    int retry_count = 0;
    const int max_retries = 3;

    while (retry_count < max_retries) {
        auto result = co_await client.get("/api/data").timeout(10000ms);

        if (!result) {
            if (result.error().code() == kRequestTimeOut) {
                retry_count++;
                std::cout << "Request timeout, retry " << retry_count << std::endl;
                continue;
            }
            break;
        }

        if (result.value().has_value()) {
            auto& response = result.value().value();
            std::cout << "Success: " << response.getBodyStr() << std::endl;
            break;
        }
    }

    co_await client.close();
}
```

## 总结

galay-http 现在为所有异步操作提供了统一、易用的超时支持：

- ✅ **全面覆盖**：所有 Awaitable 类型都支持超时
- ✅ **易于使用**：简单的 `.timeout()` 链式调用
- ✅ **精确可靠**：超时精度高，误差小
- ✅ **类型安全**：编译时类型检查
- ✅ **零开销**：不使用时无性能损失

使用方式：`co_await awaitable.timeout(std::chrono::milliseconds(timeout_ms))`
