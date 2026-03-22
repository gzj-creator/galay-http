# `galay-http` Non-SSL HTTP/WS/H2C Parallel Optimization Design

**Goal:** 在只修改 `galay-http`、不改任何 `CMakeLists.txt`、不回退 awaitable 架构的前提下，并行推进 `http`、`ws`、`h2c` 三条非 SSL 热路径优化，并保留严格 TDD 与 fresh Release benchmark 证据。

## Scope

- 只关注非 SSL 协议: `http`, `ws`, `h2c`
- 不修改 `galay-kernel` / `galay-ssl`
- 不新增协议专用 public awaitable
- 不回退到旧的协议专用 awaitable
- 不清理或回滚当前工作区已有未提交改动

## Why Parallel

这三条协议的核心热路径已经基本解耦:

- `http` 主要落在 `HttpReader.h` / `HttpWriter.h`
- `ws` 主要落在 `WsReader.h` / `WsWriter.h` / `WsSession.h`
- `h2c` 主要落在 `Http2Conn.h` / `Http2Stream*.h`

只要提前固定 ownership，并避免共享文件改动，就可以安全并行。

## Ownership Split

### HTTP Agent

Owned files:

- `galay-http/kernel/http/HttpReader.h`
- `galay-http/kernel/http/HttpWriter.h`
- `benchmark/B1-HttpServer.cc`
- `test/T73-http_nonssl_steady_state.cc`

Focus:

- 降低 steady-state 读写路径分配和容器成本
- 尽量复用读状态和 writev 布局
- benchmark 侧缓存固定响应，避免每轮循环重复构造

### WS Agent

Owned files:

- `galay-http/kernel/websocket/WsReader.h`
- `galay-http/kernel/websocket/WsWriter.h`
- `galay-http/kernel/websocket/WsSession.h`
- `benchmark/B5-WebsocketServer.cc`
- `test/T74-ws_nonssl_steady_state.cc`

Focus:

- 收紧 steady-state 消息收发路径
- 避免重复构造 frame/message 临时状态
- 收紧 partial-send / steady-state clear 逻辑

### H2C Agent

Owned files:

- `galay-http/kernel/http2/Http2Conn.h`
- `galay-http/kernel/http2/Http2Stream.h`
- `benchmark/B3-H2cServer.cc`
- `test/T75-h2c_nonssl_hotpath.cc`

Focus:

- 压平 recv/parse/dispatch 热路径
- 减少 parse 侧多余对象化、容器化与 copy
- 保持现有 public surface 不变

## Shared Rules

- 新测试文件直接放在 `test/` 下即可，`test/CMakeLists.txt` 已自动 glob，不需要改 CMake
- 每条协议都必须先写失败测试，再写最小实现，再跑 focused 回归
- benchmark 只认 fresh `Release` 结果
- 最终对比由主控串行统一重跑，避免端口和环境污染

## Verification Strategy

每个 agent 自己负责:

- 失败测试的 RED 证据
- 新测试变绿
- 协议 focused 回归

主控负责:

- 合并后重新跑 fresh `Release` benchmark
- 对比 Go / Rust / Galay 同环境吞吐
- 输出最终 gap 分析
