# T18-WebSocket 帧测试

## 测试概述

本文档记录 WebSocket Frame Parser 的单元测试结果。测试覆盖了帧解析、帧编码、UTF-8 验证、错误处理等核心功能。

## 测试目标

验证 `WsFrameParser` 类的帧处理功能，确保能够正确处理：
- 文本帧和二进制帧的解析
- 扩展长度（16 位和 64 位）
- 控制帧（Ping、Pong、Close）
- 分片帧（Fragmented frames）
- 帧编码（带掩码和不带掩码）
- UTF-8 验证
- 跨 iovec 解析
- 各种错误情况

## 测试场景

### 1. 基本帧解析测试

#### 1.1 文本帧解析
- **测试内容**：解析带掩码的文本帧
- **帧结构**：
  - FIN=1, opcode=1 (Text)
  - MASK=1, payload_len=5
  - 掩码密钥：0x12, 0x34, 0x56, 0x78
  - Payload: "Hello"（经过掩码）
- **验证点**：
  - 帧头正确解析
  - 掩码正确应用
  - Payload 正确解码为 "Hello"

#### 1.2 二进制帧解析
- **测试内容**：解析二进制帧
- **帧结构**：
  - FIN=1, opcode=2 (Binary)
  - Payload: `\x01\x02\x03\x04`
- **验证点**：
  - opcode 识别为 Binary
  - 二进制数据正确解析

### 2. 扩展长度测试

#### 2.1 16 位扩展长度
- **测试内容**：解析 126 字节的 payload
- **帧结构**：
  - payload_len=126（触发 16 位扩展）
  - 扩展长度字段：0x00 0x7E（126）
- **验证点**：
  - 正确读取 16 位扩展长度
  - Payload 长度为 126 字节
  - 数据完整

#### 2.2 64 位扩展长度
- **测试内容**：解析 65536 字节的 payload
- **帧结构**：
  - payload_len=127（触发 64 位扩展）
  - 扩展长度字段：8 字节
- **验证点**：
  - 正确读取 64 位扩展长度
  - Payload 长度为 65536 字节
  - 大数据量正确处理

### 3. 控制帧测试

#### 3.1 Ping 帧
- **测试内容**：解析 Ping 帧
- **帧结构**：
  - FIN=1, opcode=9 (Ping)
  - Payload: "ping"
- **验证点**：
  - opcode 识别为 Ping
  - Payload 正确

#### 3.2 Pong 帧
- **测试内容**：解析 Pong 帧
- **帧结构**：
  - FIN=1, opcode=10 (Pong)
  - Payload: "pong"
- **验证点**：
  - opcode 识别为 Pong
  - Payload 正确

#### 3.3 Close 帧
- **测试内容**：解析 Close 帧
- **帧结构**：
  - FIN=1, opcode=8 (Close)
  - Payload: 关闭码 1000（Normal）
- **验证点**：
  - opcode 识别为 Close
  - 关闭码正确解析

### 4. 分片帧测试

#### 4.1 第一个分片
- **测试内容**：解析第一个分片帧
- **帧结构**：
  - FIN=0, opcode=1 (Text)
  - Payload: "Hello"
- **验证点**：
  - FIN=false 表示未完成
  - opcode 为 Text

#### 4.2 后续分片
- **测试内容**：解析后续分片帧
- **帧结构**：
  - FIN=1, opcode=0 (Continuation)
  - Payload: " World"
- **验证点**：
  - FIN=true 表示完成
  - opcode 为 Continuation
  - 拼接后为 "Hello World"

### 5. 帧编码测试

#### 5.1 文本帧编码（无掩码）
- **测试内容**：编码文本帧（服务器端）
- **测试代码**：
  ```cpp
  WsFrame frame = WsFrameParser::createTextFrame("Hello");
  std::string encoded = WsFrameParser::toBytes(frame, false);
  ```
- **验证点**：
  - 帧头正确：0x81（FIN=1, opcode=1）
  - 长度字段：0x05（MASK=0, len=5）
  - Payload 未掩码

#### 5.2 二进制帧编码（带掩码）
- **测试内容**：编码二进制帧（客户端）
- **测试代码**：
  ```cpp
  WsFrame frame = WsFrameParser::createBinaryFrame("Data");
  std::string encoded = WsFrameParser::toBytes(frame, true);
  ```
- **验证点**：
  - 帧头正确：0x82（FIN=1, opcode=2）
  - MASK=1
  - 包含 4 字节掩码密钥
  - Payload 已掩码

#### 5.3 控制帧编码
- **测试内容**：编码 Ping、Pong、Close 帧
- **验证点**：
  - Ping: opcode=9
  - Pong: opcode=10
  - Close: opcode=8

### 6. 往返测试（Roundtrip）

#### 6.1 文本帧往返
- **测试流程**：
  1. 创建文本帧："Hello WebSocket!"
  2. 编码为字节流（带掩码）
  3. 解码回帧对象
- **验证点**：
  - 解码后 opcode 正确
  - Payload 与原始文本相同

#### 6.2 二进制帧往返
- **测试流程**：
  1. 创建二进制帧：`\x01\x02\x03\x04\x05`
  2. 编码为字节流
  3. 解码回帧对象
- **验证点**：
  - 解码后 opcode 为 Binary
  - Payload 与原始数据相同

### 7. UTF-8 验证测试

#### 7.1 有效 UTF-8
- **测试内容**：验证有效的 UTF-8 字符串
- **测试字符串**：
  - "Hello"
  - "你好世界"
  - "Hello 世界 🌍"
- **验证点**：所有字符串通过验证

#### 7.2 无效 UTF-8
- **测试内容**：拒绝无效的 UTF-8
- **测试字符串**：
  - `\xFF\xFE`（无效字节）
  - `\xC0\x80`（过长编码）
- **验证点**：所有无效字符串被拒绝

### 8. 错误处理测试

#### 8.1 数据不完整
- **测试内容**：只提供部分帧数据
- **验证点**：
  - 返回 kWsIncomplete 错误
  - 不会崩溃

#### 8.2 缺少掩码（服务器端）
- **测试内容**：客户端帧未使用掩码
- **验证点**：
  - 返回 kWsMaskRequired 错误
  - 符合 WebSocket 规范

#### 8.3 控制帧分片
- **测试内容**：控制帧设置 FIN=0
- **验证点**：
  - 返回 kWsControlFrameFragmented 错误
  - 控制帧不允许分片

#### 8.4 保留位设置
- **测试内容**：RSV1/RSV2/RSV3 被设置
- **验证点**：
  - 返回 kWsReservedBitsSet 错误
  - 未使用扩展时保留位必须为 0

### 9. 跨 iovec 解析测试

#### 9.1 帧数据分散在多个 iovec
- **测试内容**：帧头、掩码、payload 分别在不同的 iovec
- **验证点**：
  - 正确跨越 iovec 边界读取
  - Payload 正确解析

## 测试用例列表

| 编号 | 测试用例 | 类型 | 预期结果 |
|------|---------|------|---------|
| 1 | 文本帧解析 | Parse | ✓ 解析成功 |
| 2 | 二进制帧解析 | Parse | ✓ 解析成功 |
| 3 | 16 位扩展长度 | Extended | ✓ 126 字节 |
| 4 | 64 位扩展长度 | Extended | ✓ 65536 字节 |
| 5 | Ping 帧 | Control | ✓ Ping 解析 |
| 6 | Pong 帧 | Control | ✓ Pong 解析 |
| 7 | Close 帧 | Control | ✓ Close 解析 |
| 8 | 分片帧 | Fragment | ✓ 分片正确 |
| 9 | 文本帧编码 | Encode | ✓ 编码正确 |
| 10 | 二进制帧编码 | Encode | ✓ 编码正确 |
| 11 | 控制帧编码 | Encode | ✓ 编码正确 |
| 12 | 文本帧往返 | Roundtrip | ✓ 往返成功 |
| 13 | 二进制帧往返 | Roundtrip | ✓ 往返成功 |
| 14 | 有效 UTF-8 | Validation | ✓ 验证通过 |
| 15 | 无效 UTF-8 | Validation | ✓ 验证拒绝 |
| 16 | 数据不完整 | Error | ✓ 错误检测 |
| 17 | 缺少掩码 | Error | ✓ 错误检测 |
| 18 | 控制帧分片 | Error | ✓ 错误检测 |
| 19 | 保留位设置 | Error | ✓ 错误检测 |
| 20 | 跨 iovec 解析 | Cross | ✓ 跨边界解析 |

## 测试代码位置

- **文件路径**：`/Users/gongzhijie/Desktop/projects/git/galay-http/test/T18-WebsocketFrame.cc`
- **测试函数数量**：11 个
- **代码行数**：551 行

## 运行测试

### 编译测试

```bash
cd build
cmake ..
make T18-WebsocketFrame
```

### 运行测试

```bash
./test/T18-WebsocketFrame
```

### 预期输出

```
=== WebSocket Frame Parser Unit Tests ===

Testing text frame parsing...
  ✓ Text frame parsed: "Hello"

Testing binary frame parsing...
  ✓ Binary frame parsed: 4 bytes

Testing extended length (16-bit) frame...
  ✓ Extended length (16-bit) frame parsed: 126 bytes

Testing extended length (64-bit) frame...
  ✓ Extended length (64-bit) frame parsed: 65536 bytes

Testing control frames...
  ✓ Ping frame parsed
  ✓ Pong frame parsed
  ✓ Close frame parsed

Testing fragmented frames...
  ✓ Fragmented frames parsed: "Hello" + " World"

Testing error cases...
  ✓ Incomplete data detected
  ✓ Mask required error detected
  ✓ Control frame fragmented error detected
  ✓ Reserved bits set error detected

Testing frame encoding...
  ✓ Text frame encoded (no mask): 7 bytes
  ✓ Binary frame encoded (with mask): 10 bytes
  ✓ Ping frame encoded
  ✓ Pong frame encoded
  ✓ Close frame encoded

Testing frame roundtrip (encode -> decode)...
  ✓ Text frame roundtrip: "Hello WebSocket!"
  ✓ Binary frame roundtrip: 5 bytes

Testing UTF-8 validation...
  ✓ Valid UTF-8 strings accepted
  ✓ Invalid UTF-8 strings rejected

Testing cross-iovec frame parsing...
  ✓ Cross-iovec frame parsed: "Hello"

✅ All tests passed!
```

## 测试结论

### 功能验证

✅ **帧解析完整**：支持所有 WebSocket 帧类型
✅ **扩展长度支持**：正确处理 16 位和 64 位扩展长度
✅ **掩码处理**：正确应用和验证掩码
✅ **分片支持**：支持消息分片传输
✅ **UTF-8 验证**：严格验证文本帧的 UTF-8 编码
✅ **错误检测**：全面的错误检测和报告
✅ **跨边界解析**：正确处理 iovec 边界

### WebSocket 帧格式

#### 帧头结构（2-14 字节）
```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-------+-+-------------+-------------------------------+
|F|R|R|R| opcode|M| Payload len |    Extended payload length    |
|I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
|N|V|V|V|       |S|             |   (if payload len==126/127)   |
| |1|2|3|       |K|             |                               |
+-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
|     Extended payload length continued, if payload len == 127  |
+ - - - - - - - - - - - - - - - +-------------------------------+
|                               |Masking-key, if MASK set to 1  |
+-------------------------------+-------------------------------+
| Masking-key (continued)       |          Payload Data         |
+-------------------------------- - - - - - - - - - - - - - - - +
:                     Payload Data continued ...                :
+ - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
|                     Payload Data continued ...                |
+---------------------------------------------------------------+
```

#### Opcode 定义
- 0x0: Continuation
- 0x1: Text
- 0x2: Binary
- 0x8: Close
- 0x9: Ping
- 0xA: Pong

#### Payload 长度编码
- 0-125: 直接在 7 位字段中
- 126: 后跟 16 位扩展长度
- 127: 后跟 64 位扩展长度

### 掩码机制

#### 客户端到服务器
- **必须使用掩码**：MASK=1
- **掩码密钥**：4 字节随机值
- **掩码算法**：`payload[i] ^= mask_key[i % 4]`

#### 服务器到客户端
- **不使用掩码**：MASK=0
- **原因**：防止缓存污染攻击

### 分片机制

#### 消息分片
```
// 第一个分片
FIN=0, opcode=Text, payload="Hello"

// 中间分片
FIN=0, opcode=Continuation, payload=" "

// 最后分片
FIN=1, opcode=Continuation, payload="World"
```

#### 控制帧规则
- 控制帧不能分片（FIN 必须为 1）
- 控制帧可以插入在分片消息中间
- 控制帧 payload 长度 ≤ 125 字节

### 使用示例

#### 创建和发送帧
```cpp
// 创建文本帧
WsFrame frame = WsFrameParser::createTextFrame("Hello");

// 编码（客户端使用掩码）
std::string encoded = WsFrameParser::toBytes(frame, true);

// 发送
co_await socket.send(encoded.data(), encoded.size());
```

#### 接收和解析帧
```cpp
// 从 iovec 解析
std::vector<iovec> iovecs = ring_buffer.readableIOVec();
WsFrame frame;
auto result = WsFrameParser::fromIOVec(iovecs, frame, true);

if (result.has_value()) {
    size_t consumed = result.value();
    ring_buffer.consume(consumed);

    // 处理帧
    if (frame.header.opcode == WsOpcode::Text) {
        std::cout << "Received: " << frame.payload << std::endl;
    }
}
```

### 最佳实践

1. **客户端必须使用掩码**：符合 RFC 6455 规范
2. **验证 UTF-8**：文本帧必须是有效的 UTF-8
3. **处理分片**：支持大消息的分片传输
4. **响应 Ping**：收到 Ping 立即回复 Pong
5. **优雅关闭**：发送 Close 帧并等待响应

### 性能特点

- **零拷贝解析**：直接从 iovec 读取
- **增量解析**：支持流式处理
- **高效掩码**：优化的掩码算法
- **内存友好**：不复制 payload 数据

---

**测试日期**：2026-01-29
**测试人员**：galay-http 开发团队
**文档版本**：v1.0
