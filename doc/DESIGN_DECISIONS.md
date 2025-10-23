# 设计决策文档

本文档记录了 galay-http 中的重要设计决策和原因。

## 1. Reader/Writer 生命周期管理

### 问题
在协程环境中，临时对象的生命周期管理非常关键。如果在方法内部创建临时的 Reader/Writer 并返回协程，会导致悬空引用和崩溃。

### 错误设计示例
```cpp
// ❌ 错误：在 Connection 中提供便捷方法
class WsConnection {
    AsyncResult<WsFrame, WsError> receiveFrame(settings) {
        auto reader = getReader(settings);  // 临时对象
        return reader.readFrame();          // 返回协程，但 reader 即将销毁！
    }
};

// 使用时会崩溃
co_await wsConn.receiveFrame(settings);  // ❌ 崩溃！
```

**崩溃原因：**
1. `reader` 是临时对象，在 `receiveFrame()` 返回后立即被销毁
2. `readFrame()` 返回的协程持有对 `reader` 内部成员（如 `m_socket`, `m_generator`）的引用
3. 当协程恢复执行时，访问已销毁对象的成员 → **EXC_BAD_ACCESS**

### 正确设计
```cpp
// ✅ 正确：仅提供工厂方法
class WsConnection {
    WsReader getReader(const WsSettings& params);
    WsWriter getWriter(const WsSettings& params);
};

// 使用时由用户管理生命周期
auto reader = wsConn.getReader(settings);  // 显式创建
auto writer = wsConn.getWriter(settings);  // 生命周期在整个函数

while (!wsConn.isClosed()) {
    co_await reader.readFrame();  // ✅ 安全
    co_await writer.sendText("Hello");  // ✅ 安全
}
```

### 适用类
此设计原则适用于：
- `HttpConnection` → `HttpReader` / `HttpWriter`
- `WsConnection` → `WsReader` / `WsWriter`

## 2. 协议升级在 Writer 中实现

### 问题
`HttpConnection::upgrade()` 内部创建临时 `HttpWriter` 发送升级响应，导致同样的生命周期问题。

### 错误设计
```cpp
// ❌ 错误：在 Connection 中实现升级
AsyncResult<void, HttpError> HttpConnection::upgrade(HttpRequest& request) {
    auto writer = getResponseWriter({});  // 临时对象
    auto response = createUpgradeResponse(request);
    return writer.reply(response);  // writer 即将销毁！
}

// 使用时崩溃
co_await conn.upgrade(request, {});  // ❌ 崩溃！
```

### 正确设计
```cpp
// ✅ 正确：在 Writer 中实现升级
class HttpWriter {
    AsyncResult<void, HttpError> upgradeToWebSocket(HttpRequest& request, timeout);
};

// 使用时显式创建 Writer
auto writer = conn.getResponseWriter({});  // 显式创建
co_await writer.upgradeToWebSocket(request);  // ✅ 安全
```

### 优点
1. **生命周期清晰**：用户显式管理 `HttpWriter` 的生命周期
2. **符合设计原则**：所有 I/O 操作都在 Reader/Writer 中
3. **避免崩溃**：不会创建临时对象

## 3. 移动语义和 Friend Class

### WsConnection 从 HttpConnection 创建

```cpp
class HttpConnection {
    friend class WsConnection;  // 允许访问私有成员
private:
    AsyncTcpSocket m_socket;
    TimerGenerator m_generator;
};

class WsConnection {
    static WsConnection from(HttpConnection&& conn) {
        // 通过 move 构造转移所有权
        return WsConnection(std::move(conn));
    }
    
    WsConnection(HttpConnection&& conn)
        : m_socket(std::move(conn.m_socket)),
          m_generator(std::move(conn.m_generator)) {}
};
```

### 使用模式
```cpp
// HTTP 升级完成后
WsConnection wsConn = WsConnection::from(std::move(conn));
// conn 不能再使用，所有权已转移
```

## 4. 协程和异步结果

### AsyncResult 模式
所有异步操作返回 `AsyncResult<std::expected<T, Error>>`：

```cpp
// 发起异步操作
auto result = co_await reader.readFrame();

// 检查结果
if (!result.has_value()) {
    // 处理错误
    Error error = result.error();
    std::cout << error.message() << std::endl;
} else {
    // 使用值
    auto& frame = result.value();
}
```

### AsyncWaiter 模式
内部使用 `AsyncWaiter` 管理协程：

```cpp
AsyncResult<T, E> someAsyncMethod() {
    auto waiter = std::make_shared<AsyncWaiter<T, E>>();
    waiter->appendTask(internalCoroutine(waiter, ...));
    return waiter->wait();
}

Coroutine<nil> internalCoroutine(shared_ptr<AsyncWaiter<T, E>> waiter, ...) {
    // 执行异步操作
    auto result = co_await someOperation();
    
    // 通知 waiter
    waiter->notify(result);
    co_return nil{};
}
```

## 5. 超时管理

### 两级超时设置
```cpp
// 1. 全局默认超时（在 Settings 中）
WsSettings settings;
settings.recv_timeout = std::chrono::milliseconds(30000);
settings.send_timeout = std::chrono::milliseconds(5000);

// 2. 每次操作的超时（可选）
co_await reader.readFrame(std::chrono::milliseconds(10000));  // 覆盖默认值
co_await reader.readFrame();  // 使用 settings 中的默认值
```

### TimerGenerator 使用
```cpp
auto timer = m_generator.createTimer(timeout);
auto [result, timer_result] = co_await WaitAny{
    m_socket.recv(...),
    timer.wait()
};

if (timer_result.has_value()) {
    // 超时
    waiter->notify(std::unexpected(Error(kError_Timeout)));
}
```

## 6. 错误处理层次

### 三层错误系统
```cpp
// 1. CommonError - 通用错误（网络、系统）
AsyncResult<void, CommonError> socket.close();

// 2. HttpError - HTTP 协议错误
AsyncResult<HttpResponse, HttpError> reader.getResponse();

// 3. WsError - WebSocket 协议错误
AsyncResult<WsFrame, WsError> reader.readFrame();
```

### 错误转换
```cpp
// WebSocket 错误可以转换为关闭码
WsError error = ...;
WsCloseCode code = error.toWsCloseCode();
co_await writer.sendClose(code, error.message());
```

## 7. 缓冲区管理

### Buffer 复用
Reader 使用内部缓冲区避免频繁分配：

```cpp
class WsReader {
private:
    Buffer m_buffer;  // 持久缓冲区
    
    Coroutine<nil> readFrame(...) {
        // 复用缓冲区
        if (m_buffer.capacity() < required_size) {
            m_buffer.resize(required_size);
        }
        
        // 读取数据到缓冲区
        co_await m_socket.recv(m_buffer.data(), ...);
        
        // 处理完后调整缓冲区
        m_buffer = Buffer(m_buffer.data() + consumed, remaining);
    }
};
```

## 8. WebSocket 帧处理

### 掩码规则
- **客户端→服务器**：必须掩码
- **服务器→客户端**：不掩码

```cpp
// 服务器端
WsFrame frame = WsFrame::createTextFrame(text, false);  // mask=false

// 客户端
WsFrame frame = WsFrame::createTextFrame(text, true);   // mask=true
```

### 控制帧优先级
控制帧（Close, Ping, Pong）可以插入数据帧之间：

```cpp
while (receiving_fragmented_message) {
    auto frame = co_await reader.readFrame();
    
    if (isControlFrame(frame.opcode())) {
        // 立即处理控制帧
        handleControlFrame(frame);
        continue;  // 继续接收数据帧
    }
    
    // 处理数据帧
    assembleMessage(frame);
}
```

## 9. 日志和调试

### 分级日志
```cpp
HttpLogger::getInstance()->getLogger()->getSpdlogger()
    ->set_level(spdlog::level::debug);  // trace/debug/info/warn/error
```

### 关键日志点
- HTTP 请求/响应
- WebSocket 升级
- 帧接收/发送
- 错误和异常

## 10. 测试和示例

### 测试文件结构
```
test/
  ├── test_http_server.cc     - HTTP 服务器测试
  ├── test_http_client.cc     - HTTP 客户端测试
  ├── test_ws_server.cc       - WebSocket 服务器测试
  └── test_ws_browser.html    - 浏览器测试页面
```

### 最小可用示例
参见各测试文件中的实现。

## 总结

这些设计决策的核心原则：

1. **显式优于隐式**：让用户明确管理对象生命周期
2. **避免临时对象**：在协程环境中特别重要
3. **职责分离**：Connection 管理连接，Reader/Writer 处理 I/O
4. **错误分层**：不同层次使用不同的错误类型
5. **性能优化**：缓冲区复用，减少分配

这些原则确保了代码的**正确性**、**性能**和**可维护性**。

