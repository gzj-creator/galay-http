# galay-http Full Awaitable Rollout Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Complete the `galay-http` awaitable convergence, remove redundant protocol-local socket awaitable scaffolding, and verify the full repository through examples, tests, and benchmarks before continuing to the remaining `galay-*` repositories.

**Architecture:** Treat `galay-kernel` `AwaitableBuilder` / `StateMachineAwaitable` and `galay-ssl` `SslAwaitableBuilder` / `SslStateMachineAwaitable` as the default transport orchestration layers. Keep mailbox, waiter, and lifecycle awaitables only where they are true coordination primitives. Work from surface contract to protocol implementation to repository-wide verification.

**Tech Stack:** C++23, `galay-kernel`, `galay-ssl`, CMake, repository compile-time surface tests, runtime protocol tests, examples, and benchmarks.

---

### Task 1: Lock the repository surface contract

**Files:**
- Modify: `test/T45-h2_awaitable_surface.cc`
- Modify: `test/T48-non_ssl_kernel_surface.cc`
- Modify: `test/T57-ssl_kernel_surface.cc`

**Step 1: Tighten the non-SSL surface assertions**

Expand `test/T48-non_ssl_kernel_surface.cc` so HTTP / WS / H2c socket-facing APIs are asserted against the shared kernel awaitable family and not old protocol-local concrete type names.

**Step 2: Tighten the SSL surface assertions**

Expand `test/T57-ssl_kernel_surface.cc` so HTTPS / WSS / H2 TLS socket-facing APIs are asserted against the shared SSL awaitable family.

**Step 3: Keep explicit exceptions explicit**

In `test/T45-h2_awaitable_surface.cc`, retain checks for coordination awaitables that are intentionally out of scope:

- `Http2StreamManager::*Awaitable`
- waiter-based completion awaitables
- mailbox delivery awaitables

Do not force those into the shared state-machine contract.

**Step 4: Run the surface targets**

Run:

```bash
cmake -S . -B build-full-rollout -DCMAKE_BUILD_TYPE=Release -DGALAY_HTTP_ENABLE_SSL=ON
cmake --build build-full-rollout --target T45-h2_awaitable_surface T48-non_ssl_kernel_surface T57-ssl_kernel_surface --parallel 4
```

Expected:

- compile errors should point only at APIs still exposing the wrong awaitable families

**Step 5: Commit**

```bash
git add test/T45-h2_awaitable_surface.cc test/T48-non_ssl_kernel_surface.cc test/T57-ssl_kernel_surface.cc
git commit -m "test: 锁定 galay-http awaitable 新表面"
```

### Task 2: Finish HTTP writer and session send-path cutover

**Files:**
- Modify: `galay-http/kernel/http/HttpWriter.h`
- Modify: `galay-http/kernel/http/HttpSession.h`
- Modify: `test/T48-non_ssl_kernel_surface.cc`
- Test: `test/T3-reader_writer_server.cc`
- Test: `test/T4-reader_writer_client.cc`
- Test: `test/T5-http_server.cc`
- Test: `test/T6-http_client_awaitable.cc`

**Step 1: Write or tighten the failing writer surface assertions**

Cover:

- `HttpWriterImpl<...>::sendRequest(...)`
- `HttpWriterImpl<...>::sendResponse(...)`
- `HttpWriterImpl<...>::sendChunk(...)`
- `HttpSessionImpl<...>::sendRequest(...)`
- `HttpSessionImpl<...>::sendChunk(...)`

**Step 2: Remove protocol-local send-path scaffolding**

Refactor write paths so they directly return builder/state-machine awaitables for:

- send
- writev
- local finish

Use `writev` where the writer already maintains `IoVecCursor` / scatter-gather state.

**Step 3: Run focused behavior tests**

Run:

```bash
cmake --build build-full-rollout --target T3-reader_writer_server T4-reader_writer_client T5-http_server T6-http_client_awaitable --parallel 4
```

Then run:

```bash
./build-full-rollout/bin/T3-reader_writer_server
./build-full-rollout/bin/T4-reader_writer_client
./build-full-rollout/bin/T5-http_server
./build-full-rollout/bin/T6-http_client_awaitable
```

Expected: all PASS with no write-path regressions.

**Step 4: Commit**

```bash
git add galay-http/kernel/http/HttpWriter.h galay-http/kernel/http/HttpSession.h test/T48-non_ssl_kernel_surface.cc
git commit -m "refactor(http): 收敛 writer 与 session 发送等待体"
```

### Task 3: Finish HTTP reader and request-response flow cutover

**Files:**
- Modify: `galay-http/kernel/http/HttpReader.h`
- Modify: `galay-http/kernel/http/HttpSession.h`
- Modify: `test/T48-non_ssl_kernel_surface.cc`
- Test: `test/T6-http_client_awaitable.cc`
- Test: `test/T7-http_client_awaitable_edge_cases.cc`
- Test: `test/T8-http_methods.cc`
- Test: `test/T9-chunked_server.cc`
- Test: `test/T10-chunked_client.cc`

**Step 1: Tighten read-path surface assertions**

Cover:

- `getRequest(...)`
- `getResponse(...)`
- `getChunk(...)`
- session request-response helpers

**Step 2: Replace residual custom read/rearm loops**

Model parsing through:

- `recv(...)` or `readv(...)`
- local parse
- `ParseStatus::kNeedMore`
- complete / fail

Keep `HttpError` mapping protocol-local; do not leak raw `IOError`.

**Step 3: Run focused read-path tests**

Run:

```bash
cmake --build build-full-rollout --target T6-http_client_awaitable T7-http_client_awaitable_edge_cases T8-http_methods T9-chunked_server T10-chunked_client --parallel 4
```

Then run the produced binaries.

Expected: PASS for normal request-response, half-packet, chunked, and edge-case coverage.

**Step 4: Commit**

```bash
git add galay-http/kernel/http/HttpReader.h galay-http/kernel/http/HttpSession.h test/T48-non_ssl_kernel_surface.cc
git commit -m "refactor(http): 收敛 reader 与请求响应等待体"
```

### Task 4: Finish WebSocket write, read, and upgrade cutover

**Files:**
- Modify: `galay-http/kernel/websocket/WsWriter.h`
- Modify: `galay-http/kernel/websocket/WsReader.h`
- Modify: `galay-http/kernel/websocket/WsSession.h`
- Modify: `galay-http/kernel/websocket/WsClient.h`
- Modify: `test/T48-non_ssl_kernel_surface.cc`
- Modify: `test/T57-ssl_kernel_surface.cc`
- Test: `test/T18-ws_server.cc`
- Test: `test/T19-ws_client.cc`
- Test: `test/T20-websocket_client.cc`

**Step 1: Tighten WS surface assertions**

Cover:

- `sendText(...)`
- `sendBinary(...)`
- `getFrame(...)`
- `getMessage(...)`
- client and session upgrade paths

Cover both TCP and SSL variants.

**Step 2: Replace residual custom WS socket awaitables**

Use builder/state-machine flows for:

- frame write
- frame/message read
- upgrade request / response exchange

Delete compat glue that becomes unused.

**Step 3: Run WS tests**

Run:

```bash
cmake --build build-full-rollout --target T18-ws_server T19-ws_client T20-websocket_client --parallel 4
```

Then run the produced binaries.

Expected: PASS for WS transport, frame parsing, and upgrade paths.

**Step 4: Commit**

```bash
git add galay-http/kernel/websocket/WsWriter.h galay-http/kernel/websocket/WsReader.h galay-http/kernel/websocket/WsSession.h galay-http/kernel/websocket/WsClient.h test/T48-non_ssl_kernel_surface.cc test/T57-ssl_kernel_surface.cc
git commit -m "refactor(ws): 收敛 websocket 等待体"
```

### Task 5: Finish HTTP/2 and H2c transport cutover

**Files:**
- Modify: `galay-http/kernel/http2/Http2Conn.h`
- Modify: `galay-http/kernel/http2/H2cClient.h`
- Modify: `galay-http/kernel/http2/H2Client.h`
- Modify: `galay-http/kernel/http2/Http2Server.h`
- Modify: `galay-http/kernel/http2/Http2StreamManager.h`
- Modify: `test/T45-h2_awaitable_surface.cc`
- Modify: `test/T48-non_ssl_kernel_surface.cc`
- Modify: `test/T57-ssl_kernel_surface.cc`
- Test: `test/T25-h2c_client.cc`
- Test: `test/T25-h2c_server.cc`
- Test: `test/T27-h2_server.cc`
- Test: `test/T28-h2_client.cc`
- Test: `test/T42-h2_active_conn_api.cc`
- Test: `test/T43-h2_active_conn_preferred.cc`
- Test: `test/T44-h2c_client_shutdown.cc`
- Test: `test/T46-h2_active_conn_retire.cc`

**Step 1: Separate transport awaitables from coordination awaitables**

Retain only justified exceptions in `Http2StreamManager` and stream coordination helpers.

Transport-facing connect / frame read / frame write / upgrade flows should use shared awaitable families.

**Step 2: Converge H2c and H2 client transport flows**

Refactor:

- connect / preface / upgrade
- active connection IO loops
- frame read/write submission

Keep shutdown and mailbox semantics stable where they are coordination-only.

**Step 3: Run focused H2/H2c tests**

Run:

```bash
cmake --build build-full-rollout --target T25-h2c_client T25-h2c_server T27-h2_server T28-h2_client T42-h2_active_conn_api T43-h2_active_conn_preferred T44-h2c_client_shutdown T45-h2_awaitable_surface T46-h2_active_conn_retire --parallel 4
```

Then run the produced binaries.

Expected: PASS for H2/H2c transport and active-connection lifecycle behavior.

**Step 4: Commit**

```bash
git add galay-http/kernel/http2/Http2Conn.h galay-http/kernel/http2/H2cClient.h galay-http/kernel/http2/H2Client.h galay-http/kernel/http2/Http2Server.h galay-http/kernel/http2/Http2StreamManager.h test/T45-h2_awaitable_surface.cc test/T48-non_ssl_kernel_surface.cc test/T57-ssl_kernel_surface.cc
git commit -m "refactor(http2): 收敛 h2 与 h2c 传输等待体"
```

### Task 6: Remove dead compat code and meaningless tests

**Files:**
- Delete or modify: `galay-http/kernel/SslRecvCompatAwaitable.h`
- Modify: `CMakeLists.txt`
- Modify: affected examples/tests/benchmarks as needed

**Step 1: Find dead protocol-local awaitable remnants**

Run:

```bash
rg -n "CustomAwaitable|CompatAwaitable|AwaitableImpl|SslRecvCompatAwaitable" galay-http/kernel test examples benchmark
```

Keep only explicit coordination awaitables that still have semantic value.

**Step 2: Remove or rewrite obsolete tests**

Delete tests that only pin old concrete type names or dead compat scaffolding. Rewrite tests that still cover meaningful behavior but are expressed against obsolete implementation details.

**Step 3: Rebuild the full repository**

Run:

```bash
cmake --build build-full-rollout --parallel 4
```

Expected: full tree compiles.

**Step 4: Commit**

```bash
git add -A
git commit -m "refactor(http): 清理兼容等待体与无效测试"
```

### Task 7: Fresh examples verification

**Files:**
- Test: `examples/include/*.cpp`

**Step 1: Build all examples**

Run:

```bash
cmake -S . -B build-full-rollout-verify -DCMAKE_BUILD_TYPE=Release -DGALAY_HTTP_ENABLE_SSL=ON -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=ON -DBUILD_EXAMPLES=ON
cmake --build build-full-rollout-verify --parallel 4
```

**Step 2: Run all example targets serially**

Run each generated example binary one by one.

Expected:

- no startup crash
- no transport deadlock
- no immediate protocol failure caused by awaitable migration

**Step 3: Commit verification doc updates**

Update:

- `README.md`
- `docs/04-示例代码.md`
- `docs/05-性能测试.md`

Commit:

```bash
git add README.md docs/04-示例代码.md docs/05-性能测试.md
git commit -m "docs(http): 同步 example 验证结果"
```

### Task 8: Fresh full test verification

**Files:**
- Test: full `test/` tree

**Step 1: Run the full test suite serially or with controlled parallelism**

Run:

```bash
find build-full-rollout-verify/bin -maxdepth 1 -type f -name 'T*' | sort -V
```

Then run all test binaries with per-test timeout and log capture.

Expected: full PASS.

**Step 2: Fix any regressions before moving on**

If any test fails:

- reproduce individually
- write or adjust the failing regression test first if behavior changed incorrectly
- implement the minimal fix
- rerun the individual test
- rerun the affected protocol family

**Step 3: Commit any required fixes**

```bash
git add <touched files>
git commit -m "fix(http): 修复全量回归暴露的问题"
```

### Task 9: Fresh benchmark verification

**Files:**
- Test: full `benchmark/` tree
- Modify docs if results change materially

**Step 1: Build all benchmark targets**

Run:

```bash
cmake --build build-full-rollout-verify --target B1-HttpServer B2-HttpClient B3-H2cServer B4-H2cClient B5-WebsocketServer B6-WebsocketClient B7-WssServer B8-WssClient B10-H2cMuxServer B11-H2cMuxClient B12-H2Server B13-H2Client B14-HttpsServer --parallel 4
```

**Step 2: Run benchmarks conservatively**

Run them in a controlled environment:

- low priority
- isolated ports
- avoid CPU saturation

Expected:

- no new crash
- no obvious deadlock
- no protocol-level error explosion

**Step 3: Document benchmark observations**

If the migration changes benchmark behavior materially, update `docs/05-性能测试.md` and benchmark notes.

**Step 4: Commit**

```bash
git add docs/05-性能测试.md benchmark
git commit -m "docs(http): 记录 fresh benchmark 验证结果"
```

### Task 10: Prepare cross-repo continuation

**Files:**
- Modify or create rollout notes under `docs/plans/`

**Step 1: Summarize the finalized galay-http migration rules**

Record:

- which awaitables were removed
- which coordination awaitables remained intentionally
- which tests were rewritten or deleted
- which benchmark or example caveats matter

**Step 2: Create handoff notes for the next repositories**

Target repositories:

- `galay-redis`
- `galay-etcd`
- `galay-mysql`
- any remaining `galay-*` repositories still on old awaitable patterns

**Step 3: Commit**

```bash
git add docs/plans
git commit -m "docs(http): 总结多仓 awaitable 迁移经验"
```
