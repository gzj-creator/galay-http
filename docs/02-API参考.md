# 03-API文档

## HTTP 服务端

### HttpServer

HTTP 服务器主类，负责监听端口、接受连接、分发请求。

```cpp
class HttpServer {
public:
    explicit HttpServer(const HttpServerConfig& config);
    void start(HttpRouter&& router);
    void stop();
};
```

### HttpServerConfig

服务器配置结构：

```cpp
struct HttpServerConfig {
    std::string host = "0.0.0.0";
    uint16_t port = 8080;
    int backlog = 128;
    size_t io_scheduler_count = 0;       // IO 调度器线程数（0 = 自动）
    size_t compute_scheduler_count = 0;  // 计算调度器线程数（0 = 自动）
};
```

### HttpRouter

路由器，负责将请求分发到对应的处理器。

```cpp
class HttpRouter {
public:
    // 注册路由处理器（支持多个 HTTP 方法）
    template<HttpMethod... Methods>
    void addHandler(const std::string& path, HttpRouteHandler handler);

    // 静态文件挂载（动态查找）
    void mount(const std::string& routePrefix, const std::string& dirPath,
               const StaticFileConfig& config = StaticFileConfig());

    // 静态文件挂载（启动时注册）
    void mountHardly(const std::string& routePrefix, const std::string& dirPath,
                     const StaticFileConfig& config = StaticFileConfig());

    // Nginx 风格 try_files（静态命中优先，未命中回源代理）
    void tryFiles(const std::string& routePrefix,
                  const std::string& dirPath,
                  const std::string& upstreamHost,
                  uint16_t upstreamPort,
                  const StaticFileConfig& config = StaticFileConfig(),
                  ProxyMode mode = ProxyMode::Http);

    // 反向代理
    void proxy(const std::string& routePrefix,
               const std::string& upstreamHost,
               uint16_t upstreamPort,
               ProxyMode mode = ProxyMode::Http);
};
```

路由模式：

- 精确匹配：`/api/users`
- 参数路由：`/api/users/:id`（通过 `req.getRouteParam("id")` 获取）
- 通配符：`/static/*`

### HttpConn

HTTP 连接对象，代表一个客户端连接。

```cpp
class HttpConn {
public:
    HttpReader getReader(const HttpReaderSetting& setting = HttpReaderSetting());
    HttpWriter getWriter(const HttpWriterSetting& setting = HttpWriterSetting());

    auto close();      // 返回 awaitable，关闭底层 socket
    auto shutdown();   // 返回 awaitable，SSL shutdown（仅 SslSocket 有效）

    SocketType& getSocket();  // 获取底层 socket 引用
};
```

### HttpHandler

处理器签名：

```cpp
using HttpHandler = std::function<Coroutine(HttpConn&, HttpRequest)>;

// 示例
Coroutine myHandler(HttpConn& conn, HttpRequest req) {
    auto response = Http1_1ResponseBuilder::ok()
        .text("Hello, World!")
        .build();

    auto writer = conn.getWriter();
    while (true) {
        auto result = co_await writer.sendResponse(response);
        if (!result || result.value()) break;
    }

    co_return;
}
```

### HttpRequest

HTTP 请求对象：

```cpp
class HttpRequest {
public:
    // 请求头部访问（仅非 const 版本）
    HttpRequestHeader& header();

    // 请求体访问
    std::string getBodyStr();           // 破坏性操作，移交所有权（会清空 body）
    const std::string& bodyStr() const; // 非破坏性，推荐用于读取

    // 路由参数（由 HttpRouter 自动设置）
    std::string getRouteParam(const std::string& name,
                              const std::string& defaultValue = "") const;
    bool hasRouteParam(const std::string& name) const;
    const std::map<std::string, std::string>& routeParams() const;
    void setRouteParams(std::map<std::string, std::string>&& params);

    // 便捷方法（通过 header() 访问）
    // header().method() - 获取 HTTP 方法
    // header().uri() - 获取完整 URI
    // header().version() - 获取 HTTP 版本
    // header().headerPairs() - 获取头部键值对
};
```

### HttpResponse

HTTP 响应对象：

```cpp
class HttpResponse {
public:
    // 状态行
    void setStatus(int code);
    void setVersion(HttpVersion version);

    // 头部
    void setHeader(const std::string& name, const std::string& value);
    void addHeader(const std::string& name, const std::string& value);

    // 响应体
    void setBody(const std::string& body);
    void setBody(std::string&& body);
    void setBody(std::span<const uint8_t> body);

    // 获取
    int getStatus() const;
    const std::string& getBodyStr() const;
};
```

## HTTP 客户端

### HttpClient

HTTP 客户端主类：

```cpp
class HttpClient {
public:
    explicit HttpClient(const HttpClientConfig& config = HttpClientConfig());

    // 连接服务器（通过 URL）
    auto connect(const std::string& url);  // 返回连接 awaitable

    // 获取 Session 用于读写操作
    HttpSession getSession(size_t ring_buffer_size = 8192,
                          const HttpReaderSetting& reader_setting = HttpReaderSetting(),
                          const HttpWriterSetting& writer_setting = HttpWriterSetting());

    // 关闭连接
    auto close();
};
```

### HttpSession

HTTP 会话，用于读写 HTTP 请求和响应：

```cpp
class HttpSession {
public:
    // 获取读取器
    HttpReader getReader();

    // 获取写入器
    HttpWriter getWriter();
};

// 使用示例：
// HttpClient client;
// co_await client.connect("http://example.com:80");
// auto session = client.getSession();
// auto writer = session.getWriter();
// auto reader = session.getReader();
//
// // 发送请求
// HttpRequest request = Http1_1RequestBuilder::get("/path").build();
// co_await writer.sendRequest(request);
//
// // 接收响应
// HttpResponse response;
// co_await reader.getResponse(response);
```

## WebSocket

### WsClient

WebSocket 客户端：

```cpp
class WsClient {
public:
    WsClient();

    // 连接 WebSocket 服务器
    auto connect(const std::string& url);  // 返回连接 awaitable

    // 获取 Session 用于升级和通信
    WsSession getSession(const WsWriterSetting& writer_setting,
                        size_t ring_buffer_size = 8192,
                        const WsReaderSetting& reader_setting = WsReaderSetting());

    // 关闭连接
    auto close();
};

// 使用示例：
// WsClient client;
// co_await client.connect("ws://example.com:80/ws");
// auto session = client.getSession(WsWriterSetting::byClient());
// auto upgrader = session.upgrade();
// co_await upgrader();
// auto conn = session.getConn();
```

### WsConn

WebSocket 连接：

```cpp
class WsConn {
public:
    WsReader getReader(const WsReaderSetting& setting = WsReaderSetting());
    WsWriter getWriter(WsWriterSetting setting);  // 按值传递（内部会设置 use_mask）

    auto close();       // 返回 awaitable
    bool isServer() const;  // 是否为服务端连接
};
```

### WsReader

WebSocket 读取器：

```cpp
class WsReader {
public:
    // 读取消息（自动处理分片）
    auto getMessage(std::string& message, WsOpcode& opcode)
        -> Awaitable<std::expected<std::optional<bool>, WsError>>;

    // 读取单个帧
    auto readFrame(WsFrame& frame)
        -> Awaitable<std::expected<std::optional<bool>, WsError>>;
};
```

### WsWriter

WebSocket 写入器：

```cpp
class WsWriter {
public:
    // 发送文本消息
    auto sendText(const std::string& message)
        -> Awaitable<std::expected<bool, WsError>>;

    // 发送二进制消息
    auto sendBinary(std::span<const uint8_t> data)
        -> Awaitable<std::expected<bool, WsError>>;

    // 发送 Ping
    auto sendPing(const std::string& payload = "")
        -> Awaitable<std::expected<bool, WsError>>;

    // 发送 Pong
    auto sendPong(const std::string& payload = "")
        -> Awaitable<std::expected<bool, WsError>>;

    // 发送 Close
    auto sendClose(uint16_t code = 1000, const std::string& reason = "")
        -> Awaitable<std::expected<bool, WsError>>;
};
```

### WsOpcode

WebSocket 操作码：

```cpp
enum class WsOpcode : uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA
};
```

### WsReaderSetting

读取器配置：

```cpp
struct WsReaderSetting {
    size_t max_frame_size = 1024 * 1024;        // 1MB
    size_t max_message_size = 10 * 1024 * 1024; // 10MB
    bool auto_handle_ping = true;
    bool auto_handle_close = true;
};
```

### WsWriterSetting

写入器配置：

```cpp
struct WsWriterSetting {
    bool mask = false;  // 客户端应设为 true
    size_t max_frame_size = 1024 * 1024;  // 1MB

    static WsWriterSetting byClient() {
        return WsWriterSetting{.mask = true};
    }

    static WsWriterSetting byServer() {
        return WsWriterSetting{.mask = false};
    }
};
```

### WsUpgrade

WebSocket 升级工具：

```cpp
class WsUpgrade {
public:
    // 升级 HTTP 连接到 WebSocket
    auto upgrade(HttpConn& conn, const HttpRequest& req)
        -> Awaitable<std::optional<WsConn>>;
};
```

## HTTP/2

### H2cClient

HTTP/2 客户端（明文）：

```cpp
class H2cClient {
public:
    explicit H2cClient(const H2cClientConfig& config = H2cClientConfig(),
                      size_t ring_buffer_size = 65536);

    // 连接服务器
    auto connect(const std::string& host, uint16_t port);

    // 升级到 HTTP/2（返回 Awaitable）
    auto upgrade(const std::string& path = "/");

    // 发送 GET 请求，返回 Stream
    Http2Stream::ptr get(const std::string& path);

    // 发送 POST 请求，返回 Stream
    Http2Stream::ptr post(const std::string& path,
                         const std::string& body,
                         const std::string& content_type = "application/x-www-form-urlencoded");

    // 优雅关闭连接（返回 Awaitable）
    auto shutdown();

    bool isUpgraded() const;
};

// 使用示例：
// H2cClient client;
// co_await client.connect("127.0.0.1", 9080);
// co_await client.upgrade("/");
//
// auto stream = client.get("/api/data");
// bool finished = false;
// while (!finished) {
//     auto batch = co_await stream->getFrames(16);
//     if (!batch) break;
//     for (auto& frame : batch.value()) {
//         if (!frame) break;
//         if ((frame->isHeaders() || frame->isData()) && frame->isEndStream()) {
//             finished = true;
//             break;
//         }
//     }
// }
// auto& resp = stream->response();
```

### Http2Stream

HTTP/2 流：

```cpp
class Http2Stream {
public:
    uint32_t streamId() const;
    auto getFrame();
    auto getFrames(size_t max_count = 16);
    auto replyHeader(const std::vector<Http2HeaderField>& headers, bool end_stream = false);
    auto replyData(const std::string& data, bool end_stream = false);
    auto replyRst(Http2ErrorCode error);
};
```

### Http2Header

HTTP/2 头部：

```cpp
struct Http2Header {
    std::string name;
    std::string value;
    bool sensitive = false;  // 是否敏感（不进入动态表）
};
```

## TLS 支持

### HttpsServer

HTTPS 服务器：

```cpp
class HttpsServer {
public:
    explicit HttpsServer(const HttpsServerConfig& config);
    void start(HttpRouter&& router);
    void stop();
};

struct HttpsServerConfig {
    std::string host = "0.0.0.0";
    uint16_t port = 443;
    int backlog = 128;
    size_t io_scheduler_count = 0;
    size_t compute_scheduler_count = 0;
    HttpReaderSetting reader_setting;
    HttpWriterSetting writer_setting;
    std::string cert_path;
    std::string key_path;
    std::string ca_path;       // 可选，用于客户端证书验证
    bool verify_peer = false;
    int verify_depth = 4;
};
```

### HttpsClient

HTTPS 客户端：

```cpp
class HttpsClient {
public:
    explicit HttpsClient(IOScheduler* scheduler);

    auto connect(const std::string& host, int port)
        -> Awaitable<std::optional<HttpSession>>;
};
```

### WssClient

WebSocket over TLS 客户端：

```cpp
class WssClient {
public:
    explicit WssClient(IOScheduler* scheduler);

    auto connect(const std::string& url)
        -> Awaitable<std::optional<WsConn>>;
};
```

## Builder 工具

### Http1_1RequestBuilder

HTTP/1.1 请求构造器：

```cpp
class Http1_1RequestBuilder {
public:
    Http1_1RequestBuilder();  // 默认构造

    // 链式设置
    Http1_1RequestBuilder& method(HttpMethod method);
    Http1_1RequestBuilder& uri(const std::string& uri);
    Http1_1RequestBuilder& header(const std::string& name, const std::string& value);
    Http1_1RequestBuilder& headers(std::vector<std::pair<std::string, std::string>>&& hdrs);
    Http1_1RequestBuilder& host(const std::string& host);
    Http1_1RequestBuilder& contentType(const std::string& type);
    Http1_1RequestBuilder& userAgent(const std::string& ua);
    Http1_1RequestBuilder& connection(const std::string& conn);
    Http1_1RequestBuilder& body(const std::string& body);
    Http1_1RequestBuilder& body(std::string&& body);
    Http1_1RequestBuilder& json(const std::string& json);
    Http1_1RequestBuilder& form(const std::unordered_map<std::string, std::string>& data);

    HttpRequest build();
    HttpRequest buildMove();  // 移动语义构建

    // 便捷静态方法（返回预设 method + uri 的 builder）
    static Http1_1RequestBuilder get(const std::string& uri);
    static Http1_1RequestBuilder post(const std::string& uri);
    static Http1_1RequestBuilder put(const std::string& uri);
    static Http1_1RequestBuilder del(const std::string& uri);
    static Http1_1RequestBuilder patch(const std::string& uri);
    static Http1_1RequestBuilder head(const std::string& uri);
    static Http1_1RequestBuilder options(const std::string& uri);
};
```

### Http1_1ResponseBuilder

HTTP/1.1 响应构造器：

```cpp
class Http1_1ResponseBuilder {
public:
    Http1_1ResponseBuilder();  // 默认构造

    // 链式设置
    Http1_1ResponseBuilder& status(int code);
    Http1_1ResponseBuilder& status(HttpStatusCode code);
    Http1_1ResponseBuilder& header(const std::string& name, const std::string& value);
    Http1_1ResponseBuilder& headers(std::vector<std::pair<std::string, std::string>>&& hdrs);
    Http1_1ResponseBuilder& contentType(const std::string& type);
    Http1_1ResponseBuilder& body(const std::string& body);
    Http1_1ResponseBuilder& body(std::string&& body);

    // 便捷 body 方法（自动设置 Content-Type）
    Http1_1ResponseBuilder& text(const std::string& text);
    Http1_1ResponseBuilder& html(const std::string& html);
    Http1_1ResponseBuilder& json(const std::string& json);

    HttpResponse build();
    HttpResponse buildMove();  // 移动语义构建

    // 便捷静态方法（返回预设状态码的 builder）
    static Http1_1ResponseBuilder ok();                  // 200
    static Http1_1ResponseBuilder created();             // 201
    static Http1_1ResponseBuilder noContent();           // 204
    static Http1_1ResponseBuilder badRequest();          // 400
    static Http1_1ResponseBuilder unauthorized();        // 401
    static Http1_1ResponseBuilder forbidden();           // 403
    static Http1_1ResponseBuilder notFound();            // 404
    static Http1_1ResponseBuilder internalServerError(); // 500
};
```

## 错误处理

### HttpError

HTTP 错误类型：

```cpp
class HttpError {
public:
    enum class Code {
        CONNECTION_FAILED,
        TIMEOUT,
        PARSE_ERROR,
        INVALID_REQUEST,
        INVALID_RESPONSE,
        CLOSED
    };

    Code code() const;
    std::string message() const;
};
```

### WsError

WebSocket 错误类型：

```cpp
class WsError {
public:
    enum class Code {
        CONNECTION_CLOSED,
        PROTOCOL_ERROR,
        INVALID_FRAME,
        MESSAGE_TOO_LARGE,
        TIMEOUT
    };

    Code code() const;
    std::string message() const;
};
```

### Http2Error

HTTP/2 错误类型：

```cpp
class Http2Error {
public:
    enum class Code {
        PROTOCOL_ERROR,
        INTERNAL_ERROR,
        FLOW_CONTROL_ERROR,
        SETTINGS_TIMEOUT,
        STREAM_CLOSED,
        FRAME_SIZE_ERROR,
        REFUSED_STREAM,
        CANCEL,
        COMPRESSION_ERROR,
        CONNECT_ERROR,
        ENHANCE_YOUR_CALM,
        INADEQUATE_SECURITY,
        HTTP_1_1_REQUIRED
    };

    Code code() const;
    std::string message() const;
};
```

## 枚举类型

### HttpMethod

HTTP 方法：

```cpp
enum class HttpMethod {
    GET,
    POST,
    PUT,
    DELETE,
    HEAD,
    OPTIONS,
    PATCH,
    CONNECT,
    TRACE
};
```

### HttpVersion

HTTP 版本：

```cpp
enum class HttpVersion {
    HTTP_1_0,
    HTTP_1_1,
    HTTP_2_0
};
```

### StaticFileConfig

静态文件配置：

```cpp
struct StaticFileConfig {
    enum class TransferMode {
        MEMORY,    // 内存传输
        CHUNK,     // 分块传输
        SENDFILE,  // 零拷贝传输
        AUTO       // 自动选择（默认）
    };

    TransferMode mode = TransferMode::AUTO;
    size_t chunk_size = 8192;
    size_t memory_threshold = 1024 * 1024;  // 1MB
    size_t sendfile_threshold = 10 * 1024 * 1024;  // 10MB
    bool enable_range = true;
    bool enable_etag = true;
    bool enable_cache_control = true;
    std::string cache_control = "public, max-age=3600";
};
```
