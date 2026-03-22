# `galay-http` WSS/H2 Performance Optimization Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 收敛 `galay-http` 在同环境对比中的 `wss` 与 `h2` 性能差距，先消除 benchmark 路径与 TLS 热路径中的明显架构劣势，再用 fresh 验证和协议对比确认收益。

**Architecture:** 先把 `wss` 和 `h2` benchmark 服务端对齐到框架内部已经证明更轻的路径，再在共享 `Http2StreamManager` TLS owner loop 中减少小包发送和轮询式空转。首轮只动服务端热路径，不改公开表面、不回退到 `Coroutine`、不扩散到无关协议。

**Tech Stack:** C++23, `galay-http`, installed `galay-kernel`, installed `galay-ssl`, CMake, benchmark compare harness

---

### Task 1: 记录 focused 基线并固化复现入口

**Files:**
- Create: `benchmark/compare/protocols/run_wss_h2_focus.sh`
- Modify: `docs/plans/2026-03-22-galay-http-wss-h2-performance-optimization-design.md`

**Step 1: 写 focused 复现脚本**

脚本要求:

- 只跑 `galay/go/rust` 的 `wss` 与 `h2`
- 默认使用 `build-ssl-nolog`
- 输出单独结果目录和简要摘要

**Step 2: 运行脚本确认当前基线**

Run:

```bash
BENCH_THREADS=4 GALAY_BUILD_DIR="$PWD/build-ssl-nolog" ./benchmark/compare/protocols/run_wss_h2_focus.sh
```

Expected:

- `galay wss` 明显落后 `go/rust`
- `galay h2` 明显异常落后

**Step 3: 记录基线目录**

把结果路径补回设计文档，作为首轮优化前的对照证据。

**Step 4: Commit**

```bash
git add benchmark/compare/protocols/run_wss_h2_focus.sh docs/plans/2026-03-22-galay-http-wss-h2-performance-optimization-design.md
git commit -m "docs: 固化 wss/h2 性能优化基线"
```

### Task 2: 让 `B7-WssServer` 回到统一 WebSocket 栈

**Files:**
- Modify: `benchmark/B7-WssServer.cc`
- Test: `examples/include/E7-wss_server.cpp`
- Test: `benchmark/B8-WssClient.cc`

**Step 1: 写最小失败复现**

方式:

- 用 focused `wss` 基线脚本保留当前性能劣势证据
- 记录 `B7-WssServer.cc` 当前仍手动维护 `SslSocket + accumulated buffer + WsFrameParser` 的实现

**Step 2: 最小实现**

将 `B7-WssServer.cc` 改成与 `B5-WebsocketServer.cc` 同构:

- 升级成功后将连接转成 `WssConn`
- 使用 `WsReader/WsWriter` 发送欢迎消息、回显文本/二进制、响应 ping、发送 close
- 删除 benchmark 自己维护的帧累计和解析循环

**Step 3: 跑 focused 功能验证**

Run:

```bash
cmake --build build-ssl --target B7-WssServer B8-WssClient E7-WssServer E8-WssClient --parallel 4
./build-ssl/benchmark/B7-WssServer 8443 2 cert/test.crt cert/test.key &
srv=$!
sleep 1
./build-ssl/benchmark/B8-WssClient wss://127.0.0.1:8443/ws 5 1 64
kill $srv
wait $srv || true
```

Expected:

- `B8-WssClient` 成功完成
- 与 `E7/E8` 的行为模型保持一致

**Step 4: 跑 focused 性能复现**

Run:

```bash
BENCH_THREADS=4 GALAY_BUILD_DIR="$PWD/build-ssl-nolog" ./benchmark/compare/protocols/run_wss_h2_focus.sh
```

Expected:

- `wss` 指标优于基线，且不影响 `h2` 正确性

**Step 5: Commit**

```bash
git add benchmark/B7-WssServer.cc
git commit -m "perf: unify wss benchmark with websocket runtime path"
```

### Task 3: 让 `B12-H2Server` 对齐 `B3-H2cServer` 的批处理模式

**Files:**
- Modify: `benchmark/B12-H2Server.cc`
- Reference: `benchmark/B3-H2cServer.cc`
- Test: `benchmark/B13-H2Client.cc`
- Test: `test/T27-h2_server.cc`
- Test: `test/T28-h2_client.cc`

**Step 1: 写最小失败复现**

方式:

- 使用 focused `h2` 基线脚本保留当前低吞吐证据
- 记录 `B12-H2Server.cc` 当前仍使用 per-stream handler 和拆分式响应发送

**Step 2: 最小实现**

将 `B12-H2Server.cc` 调整为:

- 使用 `activeConnHandler`
- `ctx.getActiveStreams(64)` 批量消费 ready streams
- 对完整请求使用 `sendEncodedHeadersAndData(...)`
- 对多块 body 使用 `sendEncodedHeadersAndDataChunks(...)`
- 复用预编码 `content-length=128` 头块

**Step 3: 跑 focused 功能验证**

Run:

```bash
cmake --build build-ssl --target B12-H2Server B13-H2Client T27-h2_server T28-h2_client --parallel 4
./build-ssl/benchmark/B12-H2Server 9443 2 cert/test.crt cert/test.key &
srv=$!
sleep 1
./build-ssl/benchmark/B13-H2Client 127.0.0.1 9443 4 5 2 30 1
kill $srv
wait $srv || true
```

Expected:

- `B13-H2Client` 成功完成
- `T27/T28` 继续通过

**Step 4: 跑 focused 性能复现**

Run:

```bash
BENCH_THREADS=4 GALAY_BUILD_DIR="$PWD/build-ssl-nolog" ./benchmark/compare/protocols/run_wss_h2_focus.sh
```

Expected:

- `h2` 相比基线显著提升

**Step 5: Commit**

```bash
git add benchmark/B12-H2Server.cc
git commit -m "perf: align h2 benchmark with active-connection batch path"
```

### Task 4: 在 `Http2StreamManager` 里减少 TLS 小包发送

**Files:**
- Modify: `galay-http/kernel/http2/Http2StreamManager.h`
- Test: `benchmark/B12-H2Server.cc`
- Test: `benchmark/B13-H2Client.cc`
- Test: `test/T61-h2_tls_ssl_owner_loop.cc`
- Test: `test/T62-wss_writer_steady_state.cc`

**Step 1: 写共享热路径的失败复现**

方式:

- 使用 Task 3 后的 `h2` focused 结果作为 shared TLS 发送路径基线
- 记录 `sslServiceLoop(...)` 当前是逐 item flatten、逐 buffer `socket.send(...)`

**Step 2: 最小实现**

在 `sslServiceLoop(...)` 中:

- 单轮尽量多收集 `Http2OutgoingFrame`
- 将多个可发送片段合并成更少的连续 TLS 写入
- 保留 `SslSocket` 单 owner 约束
- 不引入新的 public awaitable surface

**Step 3: 跑 focused 回归**

Run:

```bash
cmake --build build-ssl --target T61-h2_tls_ssl_owner_loop T62-wss_writer_steady_state B12-H2Server B13-H2Client --parallel 4
./build-ssl/test/T61-h2_tls_ssl_owner_loop
./build-ssl/test/T62-wss_writer_steady_state
./build-ssl/benchmark/B12-H2Server 9443 2 cert/test.crt cert/test.key &
srv=$!
sleep 1
./build-ssl/benchmark/B13-H2Client 127.0.0.1 9443 4 5 2 30 1
kill $srv
wait $srv || true
```

Expected:

- focused tests 通过
- `B13-H2Client` 通过且吞吐继续改善或至少不回退

**Step 4: Commit**

```bash
git add galay-http/kernel/http2/Http2StreamManager.h
git commit -m "perf: batch tls outbound writes in h2 stream manager"
```

### Task 5: 调整 TLS owner loop 的空转和调度节奏

**Files:**
- Modify: `galay-http/kernel/http2/Http2StreamManager.h`
- Test: `test/T61-h2_tls_ssl_owner_loop.cc`
- Test: `benchmark/B12-H2Server.cc`
- Test: `benchmark/B13-H2Client.cc`

**Step 1: 写最小失败复现**

方式:

- 记录 `sslServiceLoop(...)` 当前使用 `5ms` timeout 轮询读取
- 使用上一任务后的 focused `h2` 数据作为 owner-loop 调优前基线

**Step 2: 最小实现**

目标:

- 优先 drain outbound
- 及时 parse inbound backlog
- 仅在确实无事可做时进入最小必要等待
- 尽量缩小固定 timeout 的存在范围

约束:

- 不改变 `SslSocket` 单 owner 模型
- 不修改 public API
- 不引入新的跨模块协议专用抽象

**Step 3: 跑 focused 性能复现**

Run:

```bash
BENCH_THREADS=4 GALAY_BUILD_DIR="$PWD/build-ssl-nolog" ./benchmark/compare/protocols/run_wss_h2_focus.sh
```

Expected:

- `h2` 再次提升，或至少能清楚证明瓶颈已不在 owner loop

**Step 4: Commit**

```bash
git add galay-http/kernel/http2/Http2StreamManager.h
git commit -m "perf: reduce tls owner loop idle overhead"
```

### Task 6: 做完整 fresh 验证并记录结果

**Files:**
- Modify: `benchmark/results/*`
- Modify: `docs/plans/2026-03-22-galay-http-wss-h2-performance-optimization-design.md`

**Step 1: 全量构建**

Run:

```bash
cmake -S . -B build-ssl -DCMAKE_BUILD_TYPE=Release -DGALAY_HTTP_ENABLE_SSL=ON
cmake --build build-ssl --parallel 8
cmake -S . -B build-ssl-nolog -DCMAKE_BUILD_TYPE=Release -DGALAY_HTTP_ENABLE_SSL=ON -DGALAY_HTTP_DISABLE_FRAMEWORK_LOG=ON
cmake --build build-ssl-nolog --parallel 8
```

**Step 2: 全量 fresh 验证**

要求:

- 当前源码定义的 `test/` 全部通过
- `examples/` 全部通过
- `benchmark/` smoke 验证全部通过

**Step 3: 跑完整协议对比**

Run:

```bash
BENCH_THREADS=4 GALAY_BUILD_DIR="$PWD/build-ssl-nolog" ./benchmark/compare/protocols/run_compare.sh
```

**Step 4: 记录结果**

更新设计文档中的:

- 新结果目录
- `wss` 改善幅度
- `h2` 改善幅度
- 仍存在的差距及下一轮候选方向

**Step 5: 最终提交**

```bash
git add galay-http benchmark docs/plans
git commit -m "perf: optimize galay-http wss and h2 tls hot paths"
```
