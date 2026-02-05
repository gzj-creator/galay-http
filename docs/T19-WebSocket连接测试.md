# T19-WebSocket 连接测试

## 测试概述

本文档记录 WebSocket 连接和升级机制的测试结果。测试覆盖了 WsConn 创建、配置、帧创建、辅助函数、错误处理等功能。

## 测试目标

验证 WebSocket 连接相关功能，确保能够正确处理：
- WsConn 对象创建
- WsReader 和 WsWriter 配置
- WebSocket 帧创建
- Opcode 辅助函数
- 关闭码处理
- 错误转换
- HttpConn 升级机制

## 测试场景

### 1. WsConn 创建测试

#### 1.1 基本创建
- **测试内容**：创建 WebSocket 连接对象
- **测试代码**：
  ```cpp
  WsReaderSetting reader_setting;
  WsWriterSetting writer_setting(false);  // 服务器端
  ```
- **验证点**：
  - WsReaderSetting 创建成功
  - WsWriterSetting 创建成功
  - 配置参数正确

### 2. WebSocket 配置测试

#### 2.1 服务器端配置
- **测试内容**：配置服务器端 WebSocket
- **配置参数**：
  ```cpp
  WsWriterSetting writer_setting(false);  // 不使用掩码
  ```
- **验证点**：
  - `use_mask = false`（服务器端不使用掩码）
  - 其他参数使用默认值

#### 2.2 客户端配置
- **测试内容**：配置客户端 WebSocket
- **配置参数**：
  ```cpp
  WsWriterSetting writer_setting(true);  // 使用掩码
  ```
- **验证点**：
  - `use_mask = true`（客户端必须使用掩码）
  - 符合 RFC 6455 规范

#### 2.3 自定义配置
- **测试内容**：设置自定义配置参数
- **配置参数**：
  ```cpp
  reader_setting.max_frame_size = 1024 * 1024;      // 1MB
  reader_setting.max_message_size = 10 * 1024 * 1024; // 10MB
  ```
- **验证点**：
  - 自定义参数正确设置
  - 参数值正确获取

### 3. 帧创建测试

#### 3.1 文本帧
- **测试内容**：创建文本帧
- **测试代码**：
  ```cpp
  WsFrame frame = WsFrameParser::createTextFrame("Hello WebSocket");
  ```
- **验证点**：
  - opcode = Text
  - FIN = true
  - payload 正确

#### 3.2 二进制帧
- **测试内容**：创建二进制帧
- **测试代码**：
  ```cpp
  WsFrame frame = WsFrameParser::createBinaryFrame(data);
  ```
- **验证点**：
  - opcode = Binary
  - payload 正确

#### 3.3 控制帧
- **测试内容**：创建 Ping、Pong、Close 帧
- **验证点**：
  - Ping 帧：opcode = Ping
  - Pong 帧：opcode = Pong
  - Close 帧：opcode = Close

### 4. Opcode 辅助函数测试

#### 4.1 控制帧检测
- **测试内容**：`isControlFrame()` 函数
- **测试用例**：
  ```cpp
  assert(isControlFrame(WsOpcode::Close) == true);
  assert(isControlFrame(WsOpcode::Ping) == true);
  assert(isControlFrame(WsOpcode::Pong) == true);
  assert(isControlFrame(WsOpcode::Text) == false);
  assert(isControlFrame(WsOpcode::Binary) == false);
  ```
- **验证点**：正确识别控制帧

#### 4.2 数据帧检测
- **测试内容**：`isDataFrame()` 函数
- **测试用例**：
  ```cpp
  assert(isDataFrame(WsOpcode::Text) == true);
  assert(isDataFrame(WsOpcode::Binary) == true);
  assert(isDataFrame(WsOpcode::Continuation) == true);
  assert(isDataFrame(WsOpcode::Close) == false);
  ```
- **验证点**：正确识别数据帧

#### 4.3 Opcode 名称
- **测试内容**：`getOpcodeName()` 函数
- **测试用例**：
  ```cpp
  assert(getOpcodeName(WsOpcode::Text) == "Text");
  assert(getOpcodeName(WsOpcode::Binary) == "Binary");
  assert(getOpcodeName(WsOpcode::Close) == "Close");
  ```
- **验证点**：返回正确的名称字符串

### 5. 关闭码测试

#### 5.1 标准关闭码
- **测试内容**：创建不同关闭码的 Close 帧
- **关闭码**：
  - Normal (1000) - 正常关闭
  - GoingAway (1001) - 端点离开
  - ProtocolError (1002) - 协议错误
  - InvalidPayload (1007) - 无效数据
- **验证点**：所有关闭码正确处理

### 6. 错误转换测试

#### 6.1 错误到关闭码转换
- **测试内容**：`WsError::toCloseCode()` 方法
- **测试用例**：
  ```cpp
  WsError error1(kWsProtocolError);
  assert(error1.toCloseCode() == WsCloseCode::ProtocolError);

  WsError error2(kWsInvalidUtf8);
  assert(error2.toCloseCode() == WsCloseCode::InvalidPayload);

  WsError error3(kWsMessageTooLarge);
  assert(error3.toCloseCode() == WsCloseCode::MessageTooBig);
  ```
- **验证点**：错误码正确映射到关闭码

### 7. HttpConn 升级机制测试

#### 7.1 升级接口
- **测试内容**：验证 HttpConn 的升级机制
- **接口说明**：
  ```cpp
  // HttpConn 提供 upgrade<>() 模板方法
  auto ws_conn = http_conn.upgrade<WsConn>(reader_setting, writer_setting);
  ```
- **验证点**：
  - 升级方法存在
  - 返回 `std::unique_ptr<WsConn>`
  - 转移 socket 和 ring_buffer 所有权

#### 7.2 升级流程
- **流程说明**：
  1. 客户端发送 WebSocket 升级请求
  2. 服务器验证升级请求
  3. 服务器返回 101 Switching Protocols
  4. HttpConn 升级为 WsConn
  5. 开始 WebSocket 通信

## 测试用例列表

| 编号 | 测试用例 | 类型 | 预期结果 |
|------|---------|------|---------|
| 1 | WsConn 创建 | Creation | ✓ 创建成功 |
| 2 | 服务器端配置 | Config | ✓ use_mask=false |
| 3 | 客户端配置 | Config | ✓ use_mask=true |
| 4 | 自定义配置 | Config | ✓ 参数正确 |
| 5 | 文本帧创建 | Frame | ✓ Text 帧 |
| 6 | 二进制帧创建 | Frame | ✓ Binary 帧 |
| 7 | 控制帧创建 | Frame | ✓ Ping/Pong/Close |
| 8 | 控制帧检测 | Helper | ✓ 检测正确 |
| 9 | 数据帧检测 | Helper | ✓ 检测正确 |
| 10 | Opcode 名称 | Helper | ✓ 名称正确 |
| 11 | 关闭码 | CloseCode | ✓ 所有码正确 |
| 12 | 错误转换 | Error | ✓ 转换正确 |
| 13 | 升级机制 | Upgrade | ✓ 接口存在 |

## 测试代码位置

- **文件路径**：`/Users/gongzhijie/Desktop/projects/git/galay-http/test/T19-WebsocketConn.cc`
- **测试函数数量**：7 个
- **代码行数**：204 行

## 运行测试

### 编译测试

```bash
cd build
cmake ..
make T19-WebsocketConn
```

### 运行测试

```bash
./test/T19-WebsocketConn
```

### 预期输出

```
=== WebSocket Connection Tests ===

Testing WsConn creation...
  ✓ WsReaderSetting created
    - max_frame_size: 1048576
    - max_message_size: 10485760
    - auto_fragment: true
  ✓ WsWriterSetting created
    - max_frame_size: 1048576
    - auto_fragment: true
    - use_mask: false

Testing WebSocket settings...
  ✓ Server-side settings: use_mask = false
  ✓ Client-side settings: use_mask = true
  ✓ Custom settings applied

Testing WebSocket frame creation...
  ✓ Text frame created
  ✓ Binary frame created
  ✓ Ping frame created
  ✓ Pong frame created
  ✓ Close frame created

Testing opcode helper functions...
  ✓ isControlFrame() works correctly
  ✓ isDataFrame() works correctly
  ✓ getOpcodeName() works correctly

Testing WebSocket close codes...
  ✓ All close codes work correctly

Testing error to close code conversion...
  ✓ Error to close code conversion works

Testing HttpConn upgrade mechanism...
  ✓ HttpConn has upgrade<>() template method
  ✓ Upgrade returns std::unique_ptr<WsConn>
  ✓ Upgrade transfers socket and ring_buffer ownership
  ℹ  Note: Actual upgrade requires runtime network connection

✅ All tests passed!

📝 Summary:
  - WsConn class created successfully
  - WsReader and WsWriter implemented
  - HttpConn upgrade mechanism added
  - WebSocket settings configurable
  - Frame creation and parsing working
```

## 测试结论

### 功能验证

✅ **WsConn 创建**：成功创建 WebSocket 连接对象
✅ **配置灵活**：支持服务器端和客户端配置
✅ **帧创建完整**：支持所有类型的帧创建
✅ **辅助函数完善**：提供便捷的 opcode 检测函数
✅ **关闭码支持**：完整支持 WebSocket 关闭码
✅ **错误处理**：错误码正确映射到关闭码
✅ **升级机制**：HttpConn 可以升级为 WsConn

### WsConn 架构

#### 类结构
```cpp
class WsConn {
public:
    WsConn(TcpSocket socket,
           RingBuffer ring_buffer,
           WsReaderSetting reader_setting,
           WsWriterSetting writer_setting,
           bool is_server);

    WsReader& getReader();
    WsWriter& getWriter();

    Coroutine close();

private:
    TcpSocket socket_;
    RingBuffer ring_buffer_;
    WsReader reader_;
    WsWriter writer_;
};
```

#### 配置参数

**WsReaderSetting**：
- `max_frame_size`：单帧最大大小（默认 1MB）
- `max_message_size`：消息最大大小（默认 10MB）
- `auto_fragment`：是否自动处理分片（默认 true）

**WsWriterSetting**：
- `max_frame_size`：单帧最大大小（默认 1MB）
- `auto_fragment`：是否自动分片（默认 true）
- `use_mask`：是否使用掩码（客户端 true，服务器 false）

### WebSocket 关闭码

| 码值 | 名称 | 说明 |
|------|------|------|
| 1000 | Normal | 正常关闭 |
| 1001 | GoingAway | 端点离开（如浏览器关闭） |
| 1002 | ProtocolError | 协议错误 |
| 1003 | UnsupportedData | 不支持的数据类型 |
| 1007 | InvalidPayload | 无效的 payload 数据 |
| 1008 | PolicyViolation | 违反策略 |
| 1009 | MessageTooBig | 消息过大 |
| 1010 | MandatoryExtension | 缺少必需的扩展 |
| 1011 | InternalError | 内部错误 |

### 升级流程示例

#### 服务器端
```cpp
Coroutine handleUpgrade(HttpConn& conn, HttpRequest req) {
    // 验证升级请求
    if (req.header().headerPairs().getValue("Upgrade") != "websocket") {
        co_return;
    }

    // 发送 101 响应
    auto response = Http1_1ResponseBuilder()
        .status(101)
        .header("Upgrade", "websocket")
        .header("Connection", "Upgrade")
        .header("Sec-WebSocket-Accept", accept_key)
        .build();

    auto writer = conn.getWriter();
    co_await writer.sendResponse(response);

    // 升级为 WebSocket
    WsReaderSetting reader_setting;
    WsWriterSetting writer_setting(false);  // 服务器端

    auto ws_conn = conn.upgrade<WsConn>(reader_setting, writer_setting);

    // WebSocket 通信
    auto ws_reader = ws_conn->getReader();
    auto ws_writer = ws_conn->getWriter();

    // 处理消息...
}
```

#### 客户端
```cpp
Coroutine connectWebSocket() {
    // 发送升级请求
    auto request = Http1_1RequestBuilder::get("/ws")
        .header("Upgrade", "websocket")
        .header("Connection", "Upgrade")
        .header("Sec-WebSocket-Version", "13")
        .header("Sec-WebSocket-Key", key)
        .build();

    HttpClient client(std::move(socket));
    auto writer = client.getWriter();
    co_await writer.sendRequest(request);

    // 读取 101 响应
    auto reader = client.getReader();
    HttpResponse response;
    co_await reader.getResponse(response);

    // 升级为 WebSocket
    WsReaderSetting reader_setting;
    WsWriterSetting writer_setting(true);  // 客户端

    WsConn ws_conn(
        std::move(client.socket()),
        std::move(client.ringBuffer()),
        reader_setting,
        writer_setting,
        false  // is_server = false
    );

    // WebSocket 通信...
}
```

### 使用建议

#### 1. 配置参数
```cpp
// 小消息场景
WsReaderSetting setting;
setting.max_frame_size = 64 * 1024;      // 64KB
setting.max_message_size = 1024 * 1024;  // 1MB

// 大消息场景
WsReaderSetting setting;
setting.max_frame_size = 10 * 1024 * 1024;   // 10MB
setting.max_message_size = 100 * 1024 * 1024; // 100MB
```

#### 2. 错误处理
```cpp
auto result = co_await ws_reader.getMessage(msg, opcode);

if (!result) {
    WsError error(result.error().code());
    WsCloseCode close_code = error.toCloseCode();

    // 发送 Close 帧
    WsFrame close_frame = WsFrameParser::createCloseFrame(
        close_code,
        result.error().message()
    );
    co_await ws_writer.sendFrame(close_frame);
}
```

#### 3. 优雅关闭
```cpp
// 发送 Close 帧
WsFrame close_frame = WsFrameParser::createCloseFrame(
    WsCloseCode::Normal,
    "Goodbye"
);
co_await ws_writer.sendFrame(close_frame);

// 等待对方的 Close 帧
std::string msg;
WsOpcode opcode;
co_await ws_reader.getMessage(msg, opcode);

// 关闭连接
co_await ws_conn.close();
```

### 性能特点

- **零拷贝**：直接从 RingBuffer 读取帧
- **增量处理**：支持流式消息处理
- **内存高效**：RingBuffer 循环使用
- **协程友好**：完全异步，不阻塞

### 标准兼容性

- **RFC 6455**：WebSocket Protocol
- **完整实现**：支持所有帧类型和控制流
- **安全性**：强制客户端使用掩码

---

**测试日期**：2026-01-29
**测试人员**：galay
**文档版本**：v1.0
