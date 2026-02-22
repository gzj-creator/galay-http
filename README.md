# Galay-HTTP

高性能 **C++23** 协程 HTTP/WebSocket/HTTP2 库，构建于 `galay-kernel` 之上。

## 特性

- C++23 协程异步模型：统一 `co_await` 风格
- HTTP/1.1：客户端、服务端、Router、静态文件挂载
- WebSocket：`ws` / `wss`
- HTTP/2：`h2c`（cleartext）
- TLS：`https` / `wss`（启用 `GALAY_HTTP_ENABLE_SSL`）
- C++23 命名模块：`galay.http` / `galay.http2` / `galay.websocket`

## 文档导航

建议从 `docs/3-使用指南.md` 开始：

1. [架构设计](docs/1-架构设计.md)
2. [API参考](docs/2-API参考.md)
3. [使用指南](docs/3-使用指南.md)
4. [性能测试](docs/04性能测试.md)

原先分散的 `B*.md` 说明已合并为单一性能测试文档，减少文档文件数量。

## 构建要求

- CMake 3.22+
- C++23 编译器（GCC 11+ / Clang 14+ / AppleClang 15+）
- `spdlog`
- `galay-kernel`
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
```

> `BUILD_MODULE_EXAMPLES` 需要 CMake `>= 3.28` 且推荐 `Ninja`/`Visual Studio` 生成器。  
> 当前 AppleClang 环境会自动关闭模块目标，避免构建失败。

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
./build/example/E1-EchoServer 8080

# 终端 2
./build/example/E2-EchoClient http://127.0.0.1:8080/echo "hello"
```

### WebSocket Echo

```bash
# 终端 1
./build/example/E3-WebsocketServer

# 终端 2
./build/example/E4-WebsocketClient ws://127.0.0.1:8080/ws
```

### H2c Echo

```bash
# 终端 1
./build/example/E9-H2cEchoServer 9080

# 终端 2
./build/example/E10-H2cEchoClient 127.0.0.1 9080
```

### 静态文件服务

```bash
./build/example/E11-StaticServer 8090 ./html
# 打开 http://127.0.0.1:8090/
```

### 反向代理

```bash
# upstream
./build/example/E1-EchoServer 8080

# proxy + mount (listen=8081, upstream=127.0.0.1:8080, /static -> ./html)
./build/example/E12-HttpProxy 8081 127.0.0.1 8080 /static ./html dynamic

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
├── example/
│   ├── common/        # 示例公共配置
│   └── include/       # 示例实现（E1~E12）
├── test/              # 测试（T*）
├── benchmark/         # 压测（B*）
└── docs/              # 主文档 + 测试/压测文档
```

## 许可证

MIT License
