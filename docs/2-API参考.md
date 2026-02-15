# 2-API参考

## 1. HTTP 服务端

核心类型：

- `HttpServer`
- `HttpServerConfig`
- `HttpRouter`
- `HttpConn`

常见流程：

1. 构造 `HttpRouter` 并注册 handler。
2. 构造 `HttpServerConfig`。
3. `HttpServer server(config);`
4. `server.start(std::move(router));`

Handler 签名：

```cpp
Coroutine handler(HttpConn& conn, HttpRequest req)
```

## 2. HTTP 客户端

核心类型：

- `HttpClient`
- `HttpSession`

常见接口（`HttpSession`）：

- `get(uri, headers)`
- `post(uri, body, content_type, headers)`
- `put(...)`
- `del(...)`
- `head(...)`
- `options(...)`
- `patch(...)`

返回值是 awaitable，完成后得到 `std::optional<HttpResponse>`。

## 3. HTTP/2（h2c）

核心类型：

- `H2cClient`
- `H2cServer`
- `Http2Stream`

典型流程（客户端）：

1. `connect(host, port)`
2. `upgrade("/")`
3. 创建/获取 stream，发送 headers + data，读取 response。

## 4. WebSocket

核心类型：

- `WsClient`
- `WsConn`
- `WsReader` / `WsWriter`

支持文本、二进制、Ping/Pong、Close。

## 5. TLS 相关

启用条件：`GALAY_HTTP_ENABLE_SSL=ON`

可用类型：

- `HttpsServer`
- `HttpsClient`
- `WssClient`

## 6. Builder

常用：

- `Http1_1RequestBuilder`
- `Http1_1ResponseBuilder`

用于快速构造请求/响应对象。
