# HTTP Connection 模块

## 1. 概述

HttpConn 是一个轻量级的连接容器，负责存储配置和底层资源（socket、buffer），不处理任何业务逻辑。

## 2. 核心组件

### 2.1 HttpConn
- **文件**: `galay-http/kernel/http/HttpConn.h/cc`
- **职责**: 封装 HTTP 连接的底层资源和配置

## 3. 设计理念

### 3.1 职责分离

**HttpConn 只负责**:
- ✅ 存储 `TcpSocket`
- ✅ 存储 `RingBuffer`
- ✅ 存储 `HttpReaderSetting` 和 `HttpWriterSetting`
- ✅ 提供工厂方法创建 `HttpReader` 和 `HttpWriter`

**HttpConn 不负责**:
- ❌ 处理业务逻辑
- ❌ 请求/响应循环
- ❌ 错误处理
- ❌ Keep-Alive 管理

### 3.2 工厂模式

HttpConn 提供工厂方法，按需创建 Reader 和 Writer：

```cpp
HttpReader getReader();   // 需要 RingBuffer
HttpWriter getWriter();   // 不需要 RingBuffer
```

**设计优势**:
1. **灵活性**: 可以在需要时创建 Reader/Writer
2. **无状态**: HttpConn 本身不持有 Reader/Writer 状态
3. **简洁性**: 接口简单清晰

## 4. 接口说明

### 4.1 构造函数

```cpp
HttpConn(TcpSocket&& socket,
         const HttpReaderSetting& reader_setting,
         const HttpWriterSetting& writer_setting);
```

### 4.2 公共方法

```cpp
// 关闭连接（返回 CloseAwaitable）
CloseAwaitable close();

// 获取底层 socket
TcpSocket& socket();

// 获取 RingBuffer
RingBuffer& ringBuffer();

// 获取 HttpReader（临时构造）
HttpReader getReader();

// 获取 HttpWriter（临时构造）
HttpWriter getWriter();
```

## 5. 使用示例

### 5.1 基本用法

```cpp
// 创建连接
HttpConn conn(std::move(socket), readerSetting, writerSetting);

// 获取 Reader 和 Writer
auto reader = conn.getReader();
auto writer = conn.getWriter();

// 读取请求
HttpRequest request;
auto result = co_await reader.getRequest(request);

// 发送响应
HttpResponse response;
// ... 构造响应 ...
auto sendResult = co_await writer.sendResponse(response);

// 关闭连接
co_await conn.close();
```

### 5.2 完整示例

```cpp
Coroutine handleConnection(TcpSocket socket) {
    // 创建连接
    HttpReaderSetting readerSetting;
    HttpWriterSetting writerSetting;
    HttpConn conn(std::move(socket), readerSetting, writerSetting);

    // 获取 Reader 和 Writer
    auto reader = conn.getReader();
    auto writer = conn.getWriter();

    // 请求/响应循环
    while (true) {
        HttpRequest request;
        auto result = co_await reader.getRequest(request);

        if (!result) {
            // 错误处理
            if (result.error().code() == kConnectionClose) {
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

        // Keep-Alive 检查
        std::string connection = request.header().headerPairs().getValue("Connection");
        if (!connection.empty() && connection == "close") {
            break;
        }

        // HTTP/1.0 默认关闭连接
        if (request.header().version() == HttpVersion::HttpVersion_1_0) {
            break;
        }
    }

    // 关闭连接
    co_await conn.close();
}
```

## 6. 成员变量

```cpp
private:
    TcpSocket m_socket;                    // 底层 socket
    RingBuffer m_ring_buffer;              // 数据缓冲区（默认 8KB）
    HttpReaderSetting m_reader_setting;    // Reader 配置
    HttpWriterSetting m_writer_setting;    // Writer 配置
```

## 7. 移动语义

HttpConn 禁用了拷贝和移动：

```cpp
// 禁用拷贝
HttpConn(const HttpConn&) = delete;
HttpConn& operator=(const HttpConn&) = delete;

// 禁用移动
HttpConn(HttpConn&&) = delete;
HttpConn& operator=(HttpConn&&) = delete;
```

**原因**:
- `HttpReader` 和 `HttpWriter` 包含引用成员
- 移动会导致引用失效
- 通常不需要移动 HttpConn

## 8. 设计优势

1. **职责清晰**: 只管理资源，不处理业务
2. **灵活使用**: 可以按需创建 Reader/Writer
3. **易于测试**: 接口简单，易于单元测试
4. **易于扩展**: 可以轻松添加新的配置项

## 9. 配置建议

### 9.1 默认配置

```cpp
HttpReaderSetting readerSetting;  // 使用默认值
HttpWriterSetting writerSetting;  // 使用默认值
HttpConn conn(std::move(socket), readerSetting, writerSetting);
```

### 9.2 自定义配置

```cpp
HttpReaderSetting readerSetting;
readerSetting.setMaxHeaderSize(16384);      // 16KB
readerSetting.setMaxBodySize(10485760);     // 10MB
readerSetting.setRecvTimeout(30000);        // 30s

HttpWriterSetting writerSetting;
writerSetting.setSendTimeout(30000);        // 30s
writerSetting.setMaxResponseSize(10485760); // 10MB

HttpConn conn(std::move(socket), readerSetting, writerSetting);
```

## 10. 测试

### 10.1 单元测试

HttpConn 本身不需要单独测试，通过 HttpReader/Writer 的测试覆盖。

### 10.2 集成测试

在 `test_reader_writer_server.cc` 中验证：

```bash
./test/test_reader_writer_server
```

### 10.3 验证点

- ✅ 正确存储和管理 socket
- ✅ 正确存储和管理 RingBuffer
- ✅ 正确存储配置
- ✅ getReader() 返回可用的 HttpReader
- ✅ getWriter() 返回可用的 HttpWriter
- ✅ close() 正确关闭连接
