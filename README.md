# Galay-HTTP

高性能 `C++23` 协程 HTTP / WebSocket / HTTP/2 库，构建于 `galay-kernel` 之上。

## 协议与支持边界

下表以真实公开头、`examples/CMakeLists.txt`、`test/CMakeLists.txt` 与 `benchmark/CMakeLists.txt` 为准：

| 能力 | 公开入口 | 构建条件 | 真实示例 / 测试 | 边界说明 |
| --- | --- | --- | --- | --- |
| HTTP/1.1 服务端 | `galay-http/kernel/http/HttpServer.h` | 默认启用 | `E1-EchoServer`、`E11-StaticServer`、`E12-HttpProxy` / `T5-http_server`、`T14-static_file_transfer_modes`、`T15-range_etag_server`、`T27-proxy_try_files_e2_e` | 标准明文 HTTP 服务面。 |
| HTTP/1.1 客户端 | `galay-http/kernel/http/HttpClient.h` | 默认启用 | `E2-EchoClient` / `T6-http_client_awaitable`、`T7-http_client_awaitable_edge_cases`、`T16-http_client_timeout` | `HttpClient::connect()` 只接受 `http://`。 |
| HTTPS | `galay-http/kernel/http/HttpServer.h`、`galay-http/kernel/http/HttpClient.h` | `-DGALAY_HTTP_ENABLE_SSL=ON` | `E5-HttpsServer`、`E6-HttpsClient` / `T21-https_server`、`T22-https_client`、`T23-https_stress_test`、`T24-simple_https_test` | `HttpsClient` 需要 `connect(url)` 之后显式 `co_await handshake()`。 |
| WebSocket 客户端 (`ws`) | `galay-http/kernel/websocket/WsClient.h`、`galay-http/kernel/websocket/WsSession.h` | 默认启用 | `E4-WebsocketClient` / `T19-ws_client`、`T20-websocket_client` | 使用 `WsClient::connect()` 后通过 `session.upgrade()` 完成协议升级。 |
| WebSocket 服务端 (`ws`) | `galay-http/kernel/http/HttpServer.h` + `galay-http/kernel/websocket/WsSession.h` | 默认启用 | `E3-WebsocketServer` / `T18-ws_server` | 仓库没有独立的 `WsServer` 公共类，服务端模式通过 HTTP Upgrade 组合实现。 |
| WSS 客户端 | `galay-http/kernel/websocket/WsClient.h`、`galay-http/kernel/websocket/WsSession.h` | `-DGALAY_HTTP_ENABLE_SSL=ON` | `E8-WssClient` | `WssClient` 需要 `connect()` 后显式 `handshake()`，再 `session.upgrade()`。 |
| WSS 服务端模式 | `galay-http/kernel/http/HttpServer.h` + `galay-http/kernel/websocket/WsUpgrade.h` | `-DGALAY_HTTP_ENABLE_SSL=ON` | `E7-WssServer` | 当前仓库没有独立 `WssServer` 公共类；`E7` 通过 `HttpsServer` + 手动帧处理工作，原因是示例注释明确指出 `SslSocket` 缺少 `readv` 支撑。 |
| HTTP/2 cleartext (`h2c`) | `galay-http/kernel/http2/Http2Server.h`、`galay-http/kernel/http2/H2cClient.h` | 默认启用 | `E9-H2cEchoServer`、`E10-H2cEchoClient` / `T26-h2c_server`、`T25-h2c_client`、`T43-h2c_client_shutdown`、`T51-h2c_server_fast_path` | 客户端序列是 `connect(host, port)` → `upgrade(path)` → `get/post`。 |
| HTTP/2 over TLS (`h2`) | `galay-http/kernel/http2/Http2Server.h`、`galay-http/kernel/http2/H2Client.h` | `-DGALAY_HTTP_ENABLE_SSL=ON` | `E13-H2EchoServer`、`E14-H2EchoClient` / `T28-h2_server`、`T29-h2_client`、`T30-h2_error_model` 至 `T53-h2_stream_pool` | `H2Client::connect(host, port)` 内部完成 TCP 连接、TLS 握手、ALPN=`h2` 校验与 preface 发送。 |
| C++23 模块目标 | `galay-http/module/*.cppm` | `-DBUILD_MODULE_EXAMPLES=ON`、CMake `>= 3.28`、`Ninja`/`Visual Studio`、非 `AppleClang` | `galay-http-modules` | 这里构建的是模块库 target，不是额外的示例二进制。 |

## 文档导航

`docs/` 的真实文件与阅读顺序如下：

1. [00-快速开始](docs/00-快速开始.md)
2. [01-架构设计](docs/01-架构设计.md)
3. [02-API参考](docs/02-API参考.md)
4. [03-使用指南](docs/03-使用指南.md)
5. [04-示例代码](docs/04-示例代码.md)
6. [05-性能测试](docs/05-性能测试.md)
7. [06-高级主题](docs/06-高级主题.md)
8. [07-常见问题](docs/07-常见问题.md)

文档总入口见 `docs/README.md`。

## 构建要求

- CMake `>= 3.22`
- `C++23` 编译器
- `spdlog`
- `galay-kernel`
- 启用 TLS 时额外需要 `galay-ssl` 与 OpenSSL

默认选项由根 `CMakeLists.txt` 定义：

- `BUILD_TESTS=ON`
- `BUILD_BENCHMARKS=ON`
- `BUILD_EXAMPLES=ON`
- `BUILD_MODULE_EXAMPLES=ON`
- `GALAY_HTTP_ENABLE_SSL=OFF`
- `GALAY_HTTP_DISABLE_FRAMEWORK_LOG=OFF`

## 构建命令

### 基础构建

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_TESTS=ON \
  -DBUILD_BENCHMARKS=ON \
  -DBUILD_MODULE_EXAMPLES=OFF
cmake --build build --parallel
```

### TLS 构建

当你需要 `https` / `wss` / `h2` 相关 API、示例、测试或 benchmark 时：

```bash
cmake -S . -B build-ssl \
  -DCMAKE_BUILD_TYPE=Release \
  -DGALAY_HTTP_ENABLE_SSL=ON \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_TESTS=ON \
  -DBUILD_BENCHMARKS=ON \
  -DBUILD_MODULE_EXAMPLES=OFF
cmake --build build-ssl --parallel
```

### 模块目标

`BUILD_MODULE_EXAMPLES` 实际控制的是 `galay-http-modules` 目标：

```bash
cmake -S . -B build-mod -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_MODULE_EXAMPLES=ON \
  -DGALAY_HTTP_ENABLE_SSL=OFF
cmake --build build-mod --target galay-http-modules --parallel
```

## 示例与验证入口

### 真实示例 target

所有示例 target 由 `examples/CMakeLists.txt` 显式定义，源码全部位于 `examples/include/`：

- 明文：`E1-EchoServer`、`E2-EchoClient`、`E3-WebsocketServer`、`E4-WebsocketClient`、`E9-H2cEchoServer`、`E10-H2cEchoClient`、`E11-StaticServer`、`E12-HttpProxy`
- TLS：`E5-HttpsServer`、`E6-HttpsClient`、`E7-WssServer`、`E8-WssClient`、`E13-H2EchoServer`、`E14-H2EchoClient`

完整映射、源码路径、运行命令与关联测试见 `docs/04-示例代码.md`。

### 常用运行命令

```bash
# HTTP echo
./build/examples/E1-EchoServer 8080
./build/examples/E2-EchoClient http://127.0.0.1:8080/echo "hello"

# h2c echo
./build/examples/E9-H2cEchoServer 9080
./build/examples/E10-H2cEchoClient 127.0.0.1 9080

# HTTPS / h2
./build-ssl/examples/E5-HttpsServer 8443 test/test.crt test/test.key
./build-ssl/examples/E6-HttpsClient https://127.0.0.1:8443/
./build-ssl/examples/E13-H2EchoServer 9443 test/test.crt test/test.key
./build-ssl/examples/E14-H2EchoClient 127.0.0.1 9443
```

### 正确的测试 target 名

`test/CMakeLists.txt` 会直接把 `test/*.cc` 文件名去掉扩展名作为 target，因此大小写与分隔符必须和文件名一致：

```bash
cmake --build build --target T26-h2c_server T25-h2c_client --parallel 4
cmake --build build-ssl --target T21-https_server T22-https_client T28-h2_server T29-h2_client --parallel 4
```

### 正确的 benchmark target 名

`benchmark/CMakeLists.txt` 同样直接使用 `benchmark/*.cc` 文件名作为 target：

```bash
cmake --build build --target B1-HttpServer B11-H2cMuxClient B15-HeaderParsing --parallel 4
cmake --build build-ssl --target B12-H2Server B13-H2Client B14-HttpsServer --parallel 4
```

## 目录结构

```text
galay-http/
├── galay-http/         # 公开头与库实现
├── examples/           # 示例源码与示例 CMake target
├── test/               # 测试源码与测试 CMake target
├── benchmark/          # benchmark 源码与 benchmark CMake target
└── docs/               # 主文档
```

## 许可证

MIT License
