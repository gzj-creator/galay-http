# `galay-http` Non-SSL HTTP/WS/H2C Parallel Optimization Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 并行优化 `galay-http` 的非 SSL `http`、`ws`、`h2c` 热路径，在不修改 CMake 和不扩展 awaitable surface 的前提下拿到 fresh 回归与 benchmark 证据。

**Architecture:** 将三条协议拆成完全独立的并行任务，每条协议只触碰自己的核心路径、benchmark 入口和新增回归测试。共享集成与最终 benchmark 由主控串行完成，避免文件冲突与端口污染。

**Tech Stack:** C++23, CMake, `galay-http`, installed `galay-kernel`, installed `galay-ssl`, benchmark compare harness

---

### Task 1: HTTP Non-SSL Steady-State Optimization

**Files:**
- Modify: `galay-http/kernel/http/HttpReader.h`
- Modify: `galay-http/kernel/http/HttpWriter.h`
- Modify: `benchmark/B1-HttpServer.cc`
- Create: `test/T73-http_nonssl_steady_state.cc`

**Step 1: 写 HTTP 失败回归测试**

目标:

- 覆盖 steady-state 多次 request/response 循环中的状态复用边界
- 失败原因必须与当前读写热路径中的重复分配、状态清理或布局重建相关

**Step 2: 运行新测试，确认先失败**

Run:

```bash
cmake --build build-wss-h2-harness --target T73-http_nonssl_steady_state --parallel 4
ctest --test-dir build-wss-h2-harness -R T73-http_nonssl_steady_state --output-on-failure
```

**Step 3: 写最小实现**

要求:

- 不新增 public API
- 不新增协议专用 awaitable
- 优先收紧 reader state / writer iovec steady-state 复用
- benchmark 侧允许缓存固定响应对象，避免每轮循环重复构造

**Step 4: 跑 focused 回归**

Run:

```bash
cmake --build build-wss-h2-harness --target T3-reader_writer_server T4-reader_writer_client T5-http_server T48-non_ssl_kernel_surface T73-http_nonssl_steady_state --parallel 4
ctest --test-dir build-wss-h2-harness -R 'T3-reader_writer_server|T4-reader_writer_client|T5-http_server|T48-non_ssl_kernel_surface|T73-http_nonssl_steady_state' --output-on-failure
```

**Step 5: 记录本协议变更与验证证据**

返回:

- 修改文件列表
- RED 失败原因
- GREEN 与 focused 回归命令输出摘要
- 如有局部 benchmark，注明端口与 fresh 数据

### Task 2: WS Non-SSL Steady-State Optimization

**Files:**
- Modify: `galay-http/kernel/websocket/WsReader.h`
- Modify: `galay-http/kernel/websocket/WsWriter.h`
- Modify: `galay-http/kernel/websocket/WsSession.h`
- Modify: `benchmark/B5-WebsocketServer.cc`
- Create: `test/T74-ws_nonssl_steady_state.cc`

**Step 1: 写 WS 失败回归测试**

目标:

- 覆盖 steady-state echo 路径中消息解析、partial-send 恢复、缓冲状态复位边界
- 失败必须是真实协议热路径问题，不允许假失败

**Step 2: 运行新测试，确认先失败**

Run:

```bash
cmake --build build-wss-h2-harness --target T74-ws_nonssl_steady_state --parallel 4
ctest --test-dir build-wss-h2-harness -R T74-ws_nonssl_steady_state --output-on-failure
```

**Step 3: 写最小实现**

要求:

- 保持当前 builder/state-machine 风格
- 不新增 public API
- 优先减少 steady-state 中的多余对象、字符串和 iovec 布局重建

**Step 4: 跑 focused 回归**

Run:

```bash
cmake --build build-wss-h2-harness --target T18-ws_server T19-ws_client T20-websocket_client T48-non_ssl_kernel_surface T50-ws_encode_into T74-ws_nonssl_steady_state --parallel 4
ctest --test-dir build-wss-h2-harness -R 'T18-ws_server|T19-ws_client|T20-websocket_client|T48-non_ssl_kernel_surface|T50-ws_encode_into|T74-ws_nonssl_steady_state' --output-on-failure
```

**Step 5: 记录本协议变更与验证证据**

返回:

- 修改文件列表
- RED 失败原因
- GREEN 与 focused 回归命令输出摘要
- 如有局部 benchmark，注明端口与 fresh 数据

### Task 3: H2C Non-SSL Hot-Path Optimization

**Files:**
- Modify: `galay-http/kernel/http2/Http2Conn.h`
- Modify: `galay-http/kernel/http2/Http2Stream.h`
- Modify: `benchmark/B3-H2cServer.cc`
- Create: `test/T75-h2c_nonssl_hotpath.cc`

**Step 1: 写 H2C 失败回归测试**

目标:

- 覆盖 recv/parse/dispatch 热路径中的 steady-state 行为边界
- 失败原因必须与 frame parse / dispatch / buffer handling 相关

**Step 2: 运行新测试，确认先失败**

Run:

```bash
cmake --build build-wss-h2-harness --target T75-h2c_nonssl_hotpath --parallel 4
ctest --test-dir build-wss-h2-harness -R T75-h2c_nonssl_hotpath --output-on-failure
```

**Step 3: 写最小实现**

要求:

- 不修改 public surface
- 不引入协议专用 public awaitable
- 优先减少 parse 侧对象化、vector materialization 和跨 iovec copy
- 避开当前未提交的 TLS/H2 owner-loop 收敛，聚焦非 SSL h2c

**Step 4: 跑 focused 回归**

Run:

```bash
cmake --build build-wss-h2-harness --target T25-h2c_server T25-h2c_client T43-h2_active_conn_preferred T44-h2c_client_shutdown T46-h2_active_conn_retire T54-h2c_server_fast_path T75-h2c_nonssl_hotpath --parallel 4
ctest --test-dir build-wss-h2-harness -R 'T25-h2c_server|T25-h2c_client|T43-h2_active_conn_preferred|T44-h2c_client_shutdown|T46-h2_active_conn_retire|T54-h2c_server_fast_path|T75-h2c_nonssl_hotpath' --output-on-failure
```

**Step 5: 记录本协议变更与验证证据**

返回:

- 修改文件列表
- RED 失败原因
- GREEN 与 focused 回归命令输出摘要
- 如有局部 benchmark，注明端口与 fresh 数据

### Task 4: 主控集成与 Fresh Benchmark

**Files:**
- Read/Verify: `benchmark/B1-HttpServer.cc`
- Read/Verify: `benchmark/B5-WebsocketServer.cc`
- Read/Verify: `benchmark/B3-H2cServer.cc`
- Read/Verify: `test/T73-http_nonssl_steady_state.cc`
- Read/Verify: `test/T74-ws_nonssl_steady_state.cc`
- Read/Verify: `test/T75-h2c_nonssl_hotpath.cc`

**Step 1: 检查三个子任务改动是否冲突**

要求:

- 仅在必要时做小范围集成修正
- 不回退任何已有未提交改动

**Step 2: 跑合并后的 focused 回归**

Run:

```bash
cmake --build build-wss-h2-harness \
  --target T3-reader_writer_server T4-reader_writer_client T5-http_server T18-ws_server T19-ws_client T20-websocket_client T25-h2c_server T25-h2c_client T43-h2_active_conn_preferred T44-h2c_client_shutdown T46-h2_active_conn_retire T48-non_ssl_kernel_surface T50-ws_encode_into T54-h2c_server_fast_path T73-http_nonssl_steady_state T74-ws_nonssl_steady_state T75-h2c_nonssl_hotpath \
  --parallel 4
ctest --test-dir build-wss-h2-harness \
  -R 'T3-reader_writer_server|T4-reader_writer_client|T5-http_server|T18-ws_server|T19-ws_client|T20-websocket_client|T25-h2c_server|T25-h2c_client|T43-h2_active_conn_preferred|T44-h2c_client_shutdown|T46-h2_active_conn_retire|T48-non_ssl_kernel_surface|T50-ws_encode_into|T54-h2c_server_fast_path|T73-http_nonssl_steady_state|T74-ws_nonssl_steady_state|T75-h2c_nonssl_hotpath' \
  --output-on-failure
```

**Step 3: 串行跑 fresh Release benchmark**

要求:

- 统一使用 `build-wss-h2-harness`
- 串行运行，避免端口冲突
- 结果必须 fresh
- 对比 Go / Rust / Galay 三套服务端

**Step 4: 输出最终结论**

必须包含:

- 三条协议分别修改了哪些文件
- 每条协议的新测试是否先失败，以及失败原因是否正确
- focused 回归 fresh 证据
- `http` / `ws` / `h2c` 的 fresh Release benchmark 结果
- 仍然存在的主要性能差距位于哪里
