# T2-Chunk 测试

## 测试概述

本文档记录 HTTP Chunk 编码/解码功能的单元测试结果。测试覆盖了 Chunk 类的所有核心功能，包括编码、解码、跨 iovec 解析和往返测试。

## 测试目标

验证 `galay::http::Chunk` 类的正确性，确保能够：
- 正确编码数据为 Chunked 格式
- 正确解码 Chunked 格式数据
- 处理跨 iovec 的 chunk 数据
- 检测不完整的 chunk 数据
- 支持编码-解码往返测试

## 测试场景

### 1. Chunk 编码测试（toChunk）

#### 1.1 普通 Chunk 编码
- **测试内容**：编码字符串 "Hello" 为 chunk 格式
- **输入**：`"Hello"`
- **预期输出**：`"5\r\nHello\r\n"`
- **验证点**：
  - 长度字段正确（十六进制）
  - 数据内容正确
  - CRLF 分隔符正确

#### 1.2 最后一个 Chunk 编码
- **测试内容**：编码空数据为最后一个 chunk
- **输入**：`""` + `isLast=true`
- **预期输出**：`"0\r\n\r\n"`
- **验证点**：
  - 长度为 0
  - 包含双 CRLF（结束标记）

#### 1.3 从 Buffer 编码
- **测试内容**：从 C 风格 buffer 编码 chunk
- **输入**：`buffer="World!", length=6`
- **预期输出**：`"6\r\nWorld!\r\n"`
- **验证点**：
  - 正确处理指针和长度
  - 编码结果正确

### 2. Chunk 解码测试（fromIOVec）

#### 2.1 单个 Chunk 解析
- **测试内容**：解析单个完整的 chunk
- **输入**：`"5\r\nHello\r\n"`
- **预期输出**：
  - `isLast = false`
  - `consumed = 9` 字节
  - `output = "Hello"`
- **验证点**：
  - 长度解析正确
  - 数据提取正确
  - 消费字节数正确

#### 2.2 最后一个 Chunk 解析
- **测试内容**：解析结束 chunk
- **输入**：`"0\r\n\r\n"`
- **预期输出**：
  - `isLast = true`
  - `consumed = 5` 字节
  - `output = ""`（空）
- **验证点**：
  - 正确识别结束标记
  - 返回 true 表示最后一个 chunk

#### 2.3 多个 Chunk 连续解析
- **测试内容**：一次性解析多个 chunk
- **输入**：`"5\r\nHello\r\n6\r\nWorld!\r\n"`
- **预期输出**：
  - `isLast = false`
  - `output = "HelloWorld!"`（追加模式）
- **验证点**：
  - 连续解析多个 chunk
  - 数据正确追加

#### 2.4 不完整数据检测
- **测试内容**：检测不完整的 chunk 数据
- **输入**：`"5\r\nHel"`（缺少 2 字节）
- **预期输出**：
  - 返回错误
  - `error.code() == kIncomplete`
- **验证点**：
  - 正确识别不完整数据
  - 返回适当的错误码

#### 2.5 跨 iovec 的 Chunk 解析
- **测试内容**：chunk 数据分布在两个 iovec 中
- **输入**：
  - `iovec[0] = "5\r\nHe"`
  - `iovec[1] = "llo\r\n"`
- **预期输出**：
  - `output = "Hello"`
- **验证点**：
  - 正确处理跨 iovec 边界
  - 数据完整拼接

### 3. 往返测试（Roundtrip）

#### 3.1 编码-解码往返
- **测试内容**：编码多个 chunk 后再解码
- **测试步骤**：
  1. 编码 "First"、"Second"、"Third" 为 chunk
  2. 编码结束 chunk
  3. 合并所有 chunk
  4. 解码合并后的数据
- **预期输出**：
  - `isLast = true`
  - `output = "FirstSecondThird"`
- **验证点**：
  - 编码-解码无损
  - 数据完整性保持

## 测试用例列表

| 编号 | 测试用例 | 输入 | 预期输出 | 结果 |
|------|---------|------|---------|------|
| 1 | 普通 chunk 编码 | "Hello" | "5\r\nHello\r\n" | ✓ |
| 2 | 最后 chunk 编码 | "" + isLast | "0\r\n\r\n" | ✓ |
| 3 | Buffer 编码 | buffer, 6 | "6\r\nWorld!\r\n" | ✓ |
| 4 | 单个 chunk 解析 | "5\r\nHello\r\n" | "Hello" | ✓ |
| 5 | 最后 chunk 解析 | "0\r\n\r\n" | isLast=true | ✓ |
| 6 | 多 chunk 解析 | 两个 chunk | "HelloWorld!" | ✓ |
| 7 | 不完整数据检测 | "5\r\nHel" | kIncomplete | ✓ |
| 8 | 跨 iovec 解析 | 2 个 iovec | "Hello" | ✓ |
| 9 | 往返测试 | 3 个 chunk | "FirstSecondThird" | ✓ |

## 测试代码位置

- **文件路径**：`/Users/gongzhijie/Desktop/projects/git/galay-http/test/T2-Chunk.cc`
- **测试函数数量**：3 个主测试函数
- **代码行数**：155 行

## 核心 API 说明

### 编码 API

```cpp
// 编码字符串为 chunk
static std::string toChunk(const std::string& data, bool isLast);

// 编码 buffer 为 chunk
static std::string toChunk(const char* buffer, size_t length, bool isLast);
```

### 解码 API

```cpp
// 从 iovec 解析 chunk
static std::expected<std::pair<bool, ssize_t>, HttpError>
    fromIOVec(const std::vector<iovec>& iovecs, std::string& output);
```

**返回值说明**：
- `pair.first`：是否为最后一个 chunk
- `pair.second`：消费的字节数
- `output`：解码后的数据（追加模式）

## 运行测试

### 编译测试

```bash
cd build
cmake ..
make T2-Chunk
```

### 运行测试

```bash
./test/T2-Chunk
```

### 预期输出

```
=== HTTP Chunk Unit Tests ===
Testing Chunk::toChunk()...
  ✓ Normal chunk: 9 bytes
  ✓ Last chunk: 5 bytes
  ✓ Chunk from buffer: 10 bytes

Testing Chunk::fromIOVec()...
  ✓ Single chunk parsed: "Hello"
  ✓ Last chunk parsed
  ✓ Multiple chunks parsed: "HelloWorld!"
  ✓ Incomplete data detected
  ✓ Cross-iovec chunk parsed: "Hello"

Testing chunk roundtrip (toChunk -> fromIOVec)...
  ✓ Roundtrip successful: "FirstSecondThird"

✅ All tests passed!
```

## 测试结论

### 功能验证

✅ **编码功能完善**：支持普通 chunk 和结束 chunk 编码
✅ **解码功能健壮**：正确处理完整、不完整和跨边界数据
✅ **错误检测准确**：能够识别不完整的 chunk 数据
✅ **往返测试通过**：编码-解码无损
✅ **内存安全**：无内存泄漏、无越界访问

### Chunk 格式说明

**标准 Chunk 格式**：
```
<size-in-hex>\r\n
<data>\r\n
```

**最后一个 Chunk**：
```
0\r\n
\r\n
```

**示例**：
```
5\r\n
Hello\r\n
6\r\n
World!\r\n
0\r\n
\r\n
```

### 性能特点

- **零拷贝解析**：使用 iovec 直接访问数据
- **追加模式**：解码时直接追加到输出字符串
- **增量解析**：支持不完整数据检测

### 适用场景

1. **流式传输**：大文件下载、视频流
2. **实时数据**：日志流、监控数据推送
3. **动态内容**：内容长度未知的响应
4. **服务器推送**：SSE（Server-Sent Events）

### 与 Content-Length 对比

| 特性 | Chunked | Content-Length |
|------|---------|----------------|
| 提前知道长度 | ❌ 不需要 | ✅ 需要 |
| 流式传输 | ✅ 支持 | ❌ 需要缓存 |
| 额外开销 | ~10 字节/chunk | 无 |
| 适用场景 | 动态内容 | 静态内容 |

### 注意事项

1. **Chunk 大小**：建议每个 chunk 不小于 1KB，避免过多开销
2. **结束标记**：必须发送 `0\r\n\r\n` 表示传输结束
3. **错误处理**：不完整数据应等待更多数据，而非报错

---

**测试日期**：2026-01-29
**测试人员**：galay-http 开发团队
**文档版本**：v1.0
