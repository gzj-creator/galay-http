# Galay-HTTP

高性能 **C++23** 协程 HTTP/WebSocket/HTTP2 库，构建于 `galay-kernel` 与 `galay-utils` 之上。

## 特性

- C++23 协程异步模型：统一 `co_await` 风格
- HTTP/1.1：客户端、服务端、Router、静态文件挂载
- WebSocket：`ws` / `wss`
- HTTP/2：`h2c`（cleartext）
- TLS：`https` / `wss`（启用 `GALAY_HTTP_ENABLE_SSL`）
- C++23 命名模块：`galay.http` / `galay.http2` / `galay.websocket`

## 文档导航

完整文档位于 `docs/` 目录，建议按以下顺序阅读：

1. [快速开始](docs/01-快速开始.md) - 安装、编译、运行第一个示例
2. [架构设计](docs/02-架构设计.md) - 理解设计理念和核心架构
3. [API 文档](docs/03-API文档.md) - 完整的 API 参考手册
4. [示例代码](docs/04-示例代码.md) - 常见使用场景的代码示例
5. [高级主题](docs/05-高级主题.md) - 性能优化、中间件、安全性
6. [常见问题](docs/06-常见问题.md) - 常见问题解答
7. [性能测试](docs/07-性能测试.md) - 性能测试数据和基准测试

更多详情请查看 [文档目录](docs/README.md)。

## 构建要求

- CMake 3.22+
- C++23 编译器（GCC 11+ / Clang 14+ / AppleClang 15+）
- `spdlog`
- `galay-kernel`
- `galay-utils`
- 可选：`galay-ssl` + OpenSSL（启用 TLS 时）

## 依赖安装（macOS / Homebrew）

```bash
brew install cmake spdlog
# 仅在开启 TLS 时需要
brew install openssl
```

## 依赖安装（Ubuntu / Debian）

```bash
sudo apt-get update
sudo apt-get install -y cmake g++ libspdlog-dev
# 仅在开启 TLS 时需要
sudo apt-get install -y libssl-dev
```

## 拉取源码（统一联调推荐）

```bash
git clone https://github.com/gzj-creator/galay-kernel.git
git clone https://github.com/gzj-creator/galay-utils.git
git clone https://github.com/gzj-creator/galay-http.git
# 可选：启用 TLS 时一并拉取
git clone https://github.com/gzj-creator/galay-ssl.git
```

## 构建

```bash
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel
```

## 常用 CMake 选项

```cmake
option(GALAY_HTTP_ENABLE_SSL "Enable SSL/TLS support (requires galay-ssl)" OFF)
option(BUILD_MODULE_EXAMPLES "Build C++23 module(import/export) support target" ON)
option(GALAY_HTTP_DISABLE_FRAMEWORK_LOG "Compile out framework logs (WS/HTTP/HTTPS/WSS/H2C/H2)" OFF)
```

> `BUILD_MODULE_EXAMPLES` 需要 CMake `>= 3.28` 且推荐 `Ninja`/`Visual Studio` 生成器。  
> 当前 AppleClang 环境会自动关闭模块目标，避免构建失败。

### 编译期关闭框架日志

当你做性能压测时，建议直接在编译期裁剪框架日志（覆盖 `WS/HTTP/HTTPS/WSS/H2C/H2`）：

```bash
cmake -S . -B build-perf \
  -DCMAKE_BUILD_TYPE=Release \
  -DGALAY_HTTP_DISABLE_FRAMEWORK_LOG=ON
cmake --build build-perf --parallel
```

说明：

- 该开关会定义 `GALAY_HTTP_DISABLE_ALL_LOG`，并将 `SPDLOG_ACTIVE_LEVEL` 设为 `SPDLOG_LEVEL_OFF`。
- 日志调用点在编译阶段被裁剪，不再产生格式化/输出开销。

## 模块接口

项目提供 3 个命名模块接口文件：

- `galay-http/module/galay.http.cppm`
- `galay-http/module/galay.http2.cppm`
- `galay-http/module/galay.websocket.cppm`

`import` 示例：

```cpp
import galay.http;
import galay.http2;
import galay.websocket;
```

### 模块支持更新（2026-02）

本次模块接口已统一为：

- `module;`
- `#include "galay-http/module/ModulePrelude.hpp"`
- `export module ...;`
- `export { #include ... }`

对应文件：

- `galay-http/module/galay.http.cppm`
- `galay-http/module/galay.http2.cppm`
- `galay-http/module/galay.websocket.cppm`
- `galay-http/module/ModulePrelude.hpp`

推荐构建（Clang 20 + Ninja）：

```bash
cmake -S . -B build-mod -G Ninja \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm@20/bin/clang++ \
  -DGALAY_HTTP_ENABLE_SSL=OFF
cmake --build build-mod --target galay-http-modules --parallel
```

## Example（精简）

只保留核心示例：协议 Echo + 静态服务器 + Proxy。

### Echo 示例

- `E1-EchoServer` / `E2-EchoClient`：HTTP Echo
- `E3-WebsocketServer` / `E4-WebsocketClient`：WebSocket Echo
- `E5-HttpsServer` / `E6-HttpsClient`：HTTPS Echo（SSL）
- `E7-WssServer` / `E8-WssClient`：WSS Echo（SSL）
- `E9-H2cEchoServer` / `E10-H2cEchoClient`：H2c Echo

### 其他核心示例

- `E11-StaticServer`：静态文件服务器
- `E12-HttpProxy`：HTTP 反向代理（支持和 `mount` 集成）

## 快速运行

### HTTP Echo

```bash
# 终端 1
./build/examples/E1-EchoServer 8080

# 终端 2
./build/examples/E2-EchoClient http://127.0.0.1:8080/echo "hello"
```

### WebSocket Echo

```bash
# 终端 1
./build/examples/E3-WebsocketServer

# 终端 2
./build/examples/E4-WebsocketClient ws://127.0.0.1:8080/ws
```

### H2c Echo

```bash
# 终端 1
./build/examples/E9-H2cEchoServer 9080

# 终端 2
./build/examples/E10-H2cEchoClient 127.0.0.1 9080
```

### HTTP/2 新用法（h2 / h2c）

`h2` 与 `h2c` 已统一为 frame-first 流接口，推荐按帧驱动消费：

```cpp
auto stream = client.get("/");
while (true) {
    auto frame_result = co_await stream->getFrame();
    if (!frame_result || !frame_result.value()) break;
    auto frame = std::move(frame_result.value());
    if ((frame->isHeaders() || frame->isData()) && frame->isEndStream()) {
        break;
    }
}
auto& resp = stream->response();
```

服务端响应建议使用：

```cpp
co_await stream->replyHeader(Http2Headers().status(200), false);
co_await stream->replyData("ok", true);
```

最小回归命令（h2c + h2）：

```bash
cmake --build build --target T25-H2cServer T25-H2cClient --parallel 4
cmake --build build-ssl --target T27-H2Server T28-H2Client --parallel 4
```

### 静态文件服务

```bash
./build/examples/E11-StaticServer 8090 ./html
# 打开 http://127.0.0.1:8090/
```

### 反向代理

```bash
# upstream
./build/examples/E1-EchoServer 8080

# proxy + mount (listen=8081, upstream=127.0.0.1:8080, /static -> ./html)
./build/examples/E12-HttpProxy 8081 127.0.0.1 8080 /static ./html dynamic

# request through proxy (falls back to upstream)
curl -X POST http://127.0.0.1:8081/echo -d "via proxy"

# request local static file (served by mount, not proxied)
curl http://127.0.0.1:8081/static/ResumeDownload.html
```

参数说明（`E12-HttpProxy`）：

```text
E12-HttpProxy [listen_port] [upstream_host] [upstream_port]
             [mount_prefix] [mount_dir] [mount_mode]

mount_mode: dynamic(默认) | hard | nginx(try_files)
关闭 mount: mount_prefix 或 mount_dir 传 none/off
```

## 项目结构

```text
galay-http/
├── galay-http/
│   ├── kernel/        # http / http2 / websocket 核心实现
│   ├── protoc/        # 协议数据结构（http/http2/websocket）
│   ├── utils/         # builder / logger / utils
│   └── module/        # C++23 命名模块接口
├── examples/
│   ├── common/        # 示例公共配置
│   └── include/       # 示例实现（E1~E12）
├── test/              # 测试（T*）
├── benchmark/         # 压测（B*）
└── docs/              # 主文档 + 测试/压测文档
```

## 许可证

MIT License
