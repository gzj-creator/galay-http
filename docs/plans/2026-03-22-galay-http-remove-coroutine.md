# galay-http Remove Coroutine Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Remove `Coroutine` completely from `galay-http`, migrate the entire repository to `Task<void>`-based async orchestration, and verify all tests, examples, and benchmarks before committing the implementation.

**Architecture:** Delete the repository-local `Coroutine` compatibility root first so remaining references fail loudly. Then convert the public handler surface, refactor internal orchestration to pure `Task<void>` plus task-aware coordination primitives, migrate repository consumers, and finish with a fresh full-suite verification gate.

**Tech Stack:** C++23 coroutines, `galay-kernel` `Task` / `Runtime` / `Scheduler`, kernel native awaitables, `AsyncWaiter`, CMake, repository tests, examples, and benchmarks.

---

### Task 1: Lock the new `Task<void>` surface and remove the vendored `Coroutine` root

**Files:**
- Modify: `test/T45-h2_awaitable_surface.cc`
- Modify: `test/T48-non_ssl_kernel_surface.cc`
- Modify: `test/T57-ssl_kernel_surface.cc`
- Delete: `galay-kernel/kernel/Coroutine.h`
- Modify: `galay-kernel/kernel/Runtime.h`

**Step 1: Write the failing surface assertions**

Add or tighten compile-time checks so public async handler surfaces require `Task<void>` rather than `Coroutine`.

```cpp
static_assert(std::is_same_v<HttpRouteHandler,
              std::function<galay::kernel::Task<void>(HttpConn&, HttpRequest)>>);
```

**Step 2: Build the surface targets and confirm they fail on remaining `Coroutine` exposure**

Run: `cmake --build build-ssl --target T45-h2_awaitable_surface T48-non_ssl_kernel_surface T57-ssl_kernel_surface --parallel 4`
Expected: FAIL on remaining `Coroutine`-typed public APIs or vendored compatibility usage.

**Step 3: Remove the vendored compatibility header**

Delete `galay-kernel/kernel/Coroutine.h` and update `galay-kernel/kernel/Runtime.h` so it only forwards to the installed runtime header.

**Step 4: Rebuild the surface targets**

Run: `cmake --build build-ssl --target T45-h2_awaitable_surface T48-non_ssl_kernel_surface T57-ssl_kernel_surface --parallel 4`
Expected: FAIL only on real repository call sites that still need migration.

**Step 5: Commit**

```bash
git add test/T45-h2_awaitable_surface.cc test/T48-non_ssl_kernel_surface.cc test/T57-ssl_kernel_surface.cc galay-kernel/kernel/Runtime.h
git rm galay-kernel/kernel/Coroutine.h
git commit -m "test: 锁定 Task 化后的公开异步表面"
```

### Task 2: Convert HTTP and WebSocket public handlers to `Task<void>`

**Files:**
- Modify: `galay-http/kernel/http/HttpServer.h`
- Modify: `galay-http/kernel/http/HttpRouter.h`
- Modify: `galay-http/kernel/http/HttpRouter.cc`
- Modify: `galay-http/kernel/http2/Http2Server.h`
- Modify: `galay-http/kernel/http2/Http2StreamManager.h`
- Modify: `examples/include/E1-echo_server.cpp`
- Modify: `examples/include/E3-websocket_server.cpp`
- Modify: `examples/include/E5-https_server.cpp`
- Modify: `examples/include/E7-wss_server.cpp`
- Modify: `examples/include/E9-h2c_echo_server.cpp`
- Modify: `examples/include/E11-static_server.cpp`
- Modify: `examples/include/E13-h2_echo_server.cpp`
- Test: `test/T5-http_server.cc`
- Test: `test/T11-http_router.cc`
- Test: `test/T12-http_router_validation.cc`
- Test: `test/T18-ws_server.cc`
- Test: `test/T21-https_server.cc`
- Test: `test/T25-h2c_server.cc`
- Test: `test/T27-h2_server.cc`

**Step 1: Write or tighten failing server-side tests**

Update test handlers to `Task<void>` and build the affected targets to surface server-side signature mismatches.

**Step 2: Change public handler typedefs and loop signatures**

Convert:

- `HttpConnHandlerImpl`
- `HttpRouteHandler`
- `Http2StreamHandler`
- `Http2ActiveConnHandler`
- `Http1FallbackHandler`

and all corresponding helper/loop return types from `Coroutine` to `Task<void>`.

**Step 3: Replace server-side `.wait()` chains with direct `co_await`**

Update HTTP/WS/H2 server code paths so child tasks are awaited directly.

**Step 4: Rebuild and run focused server-side tests**

Run: `cmake --build build-ssl --target T5-http_server T11-http_router T12-http_router_validation T18-ws_server T21-https_server T25-h2c_server T27-h2_server --parallel 4`
Expected: build succeeds.

Run: `ctest --test-dir build-ssl -R 'T5-http_server|T11-http_router|T12-http_router_validation|T18-ws_server|T21-https_server|T25-h2c_server|T27-h2_server' --output-on-failure`
Expected: PASS.

**Step 5: Commit**

```bash
git add galay-http/kernel/http/HttpServer.h galay-http/kernel/http/HttpRouter.h galay-http/kernel/http/HttpRouter.cc galay-http/kernel/http2/Http2Server.h galay-http/kernel/http2/Http2StreamManager.h examples/include/E1-echo_server.cpp examples/include/E3-websocket_server.cpp examples/include/E5-https_server.cpp examples/include/E7-wss_server.cpp examples/include/E9-h2c_echo_server.cpp examples/include/E11-static_server.cpp examples/include/E13-h2_echo_server.cpp test/T5-http_server.cc test/T11-http_router.cc test/T12-http_router_validation.cc test/T18-ws_server.cc test/T21-https_server.cc test/T25-h2c_server.cc test/T27-h2_server.cc
git commit -m "refactor: 将服务端异步 handler 全部切到 Task"
```

### Task 3: Convert clients, examples, and benchmarks to `Task<void>`

**Files:**
- Modify: `examples/include/E2-echo_client.cpp`
- Modify: `examples/include/E4-websocket_client.cpp`
- Modify: `examples/include/E6-https_client.cpp`
- Modify: `examples/include/E8-wss_client.cpp`
- Modify: `examples/include/E10-h2c_echo_client.cpp`
- Modify: `examples/include/E14-h2_echo_client.cpp`
- Modify: `benchmark/B3-H2cServer.cc`
- Modify: `benchmark/B4-H2cClient.cc`
- Modify: `benchmark/B5-WebsocketServer.cc`
- Modify: `benchmark/B6-WebsocketClient.cc`
- Modify: `benchmark/B7-WssServer.cc`
- Modify: `benchmark/B10-H2cMuxServer.cc`
- Modify: `benchmark/B11-H2cMuxClient.cc`
- Modify: `benchmark/B12-H2Server.cc`
- Modify: `benchmark/B13-H2Client.cc`
- Test: `test/T6-http_client_awaitable.cc`
- Test: `test/T7-http_client_awaitable_edge_cases.cc`
- Test: `test/T16-http_client_timeout.cc`
- Test: `test/T19-ws_client.cc`
- Test: `test/T20-websocket_client.cc`
- Test: `test/T22-https_client.cc`
- Test: `test/T24-simple_https_test.cc`
- Test: `test/T25-h2c_client.cc`
- Test: `test/T28-h2_client.cc`

**Step 1: Update client-side test handlers to `Task<void>`**

Convert client helper coroutines in tests and examples first so the build exposes remaining library-side gaps.

**Step 2: Replace residual `.wait()` usage in serial client flows**

Use direct `co_await` for child tasks and keep `runtime.spawn(...).join()` only in non-coroutine control flow.

**Step 3: Rebuild and run focused client-side tests**

Run: `cmake --build build-ssl --target T6-http_client_awaitable T7-http_client_awaitable_edge_cases T16-http_client_timeout T19-ws_client T20-websocket_client T22-https_client T24-simple_https_test T25-h2c_client T28-h2_client --parallel 4`
Expected: build succeeds.

Run: `ctest --test-dir build-ssl -R 'T6-http_client_awaitable|T7-http_client_awaitable_edge_cases|T16-http_client_timeout|T19-ws_client|T20-websocket_client|T22-https_client|T24-simple_https_test|T25-h2c_client|T28-h2_client' --output-on-failure`
Expected: PASS.

**Step 4: Build key benchmark targets**

Run: `cmake --build build-ssl --target B3-H2cServer B4-H2cClient B5-WebsocketServer B6-WebsocketClient B7-WssServer B10-H2cMuxServer B11-H2cMuxClient B12-H2Server B13-H2Client --parallel 4`
Expected: build succeeds.

**Step 5: Commit**

```bash
git add examples/include/E2-echo_client.cpp examples/include/E4-websocket_client.cpp examples/include/E6-https_client.cpp examples/include/E8-wss_client.cpp examples/include/E10-h2c_echo_client.cpp examples/include/E14-h2_echo_client.cpp benchmark/B3-H2cServer.cc benchmark/B4-H2cClient.cc benchmark/B5-WebsocketServer.cc benchmark/B6-WebsocketClient.cc benchmark/B7-WssServer.cc benchmark/B10-H2cMuxServer.cc benchmark/B11-H2cMuxClient.cc benchmark/B12-H2Server.cc benchmark/B13-H2Client.cc test/T6-http_client_awaitable.cc test/T7-http_client_awaitable_edge_cases.cc test/T16-http_client_timeout.cc test/T19-ws_client.cc test/T20-websocket_client.cc test/T22-https_client.cc test/T24-simple_https_test.cc test/T25-h2c_client.cc test/T28-h2_client.cc
git commit -m "refactor: 将客户端示例与基准切到 Task"
```

### Task 4: Refactor HTTP/2 orchestration away from `Coroutine::wait()` and `Coroutine::promise_type`

**Files:**
- Modify: `galay-http/kernel/http2/Http2StreamManager.h`
- Modify: `galay-http/kernel/http2/Http2ConnectionCore.h`
- Modify: `galay-http/kernel/http2/Http2ConnectionCore.cc`
- Modify: `galay-http/kernel/http2/H2cClient.h`
- Modify: `galay-http/kernel/http2/H2Client.h`
- Modify: `test/T42-h2_active_conn_api.cc`
- Modify: `test/T43-h2_active_conn_preferred.cc`
- Modify: `test/T44-h2c_client_shutdown.cc`
- Modify: `test/T46-h2_active_conn_retire.cc`
- Delete: `test/T63-coroutine_wait_join.cc`
- Create: `test/T63-task_background_join.cc`
- Modify: `test/CMakeLists.txt`

**Step 1: Write the failing lifecycle test**

Replace the old compatibility test with a `Task`-native lifecycle regression that covers:

- background task startup
- async completion signaling
- no scheduler-thread blocking

**Step 2: Replace stored background `Coroutine` state**

Refactor `Http2StreamManager` so writer/monitor/active handler lifecycle uses:

- scheduled or spawned `Task<void>`
- `AsyncWaiter<void>` completion signals
- task-generic wakeups via `Waker` or `TaskRef`

instead of stored `Coroutine` objects, `.wait()`, or `Coroutine::promise_type`.

**Step 3: Convert HTTP/2 control helpers to pure `Task<void>` orchestration**

Update shutdown, background start, and connection-core helpers so they no longer depend on deleted compatibility semantics.

**Step 4: Rebuild and run focused HTTP/2 lifecycle tests**

Run: `cmake --build build-ssl --target T42-h2_active_conn_api T43-h2_active_conn_preferred T44-h2c_client_shutdown T46-h2_active_conn_retire T63-task_background_join --parallel 4`
Expected: build succeeds.

Run: `ctest --test-dir build-ssl -R 'T42-h2_active_conn_api|T43-h2_active_conn_preferred|T44-h2c_client_shutdown|T46-h2_active_conn_retire|T63-task_background_join' --output-on-failure`
Expected: PASS.

**Step 5: Commit**

```bash
git add galay-http/kernel/http2/Http2StreamManager.h galay-http/kernel/http2/Http2ConnectionCore.h galay-http/kernel/http2/Http2ConnectionCore.cc galay-http/kernel/http2/H2cClient.h galay-http/kernel/http2/H2Client.h test/T42-h2_active_conn_api.cc test/T43-h2_active_conn_preferred.cc test/T44-h2c_client_shutdown.cc test/T46-h2_active_conn_retire.cc test/T63-task_background_join.cc test/CMakeLists.txt
git rm test/T63-coroutine_wait_join.cc
git commit -m "refactor: 用 Task 重写 HTTP2 后台生命周期"
```

### Task 5: Clear repository-wide `Coroutine` references from tests, docs, and build surface

**Files:**
- Modify: `galay-http/module/ModulePrelude.hpp`
- Modify: `docs/01-架构设计.md`
- Modify: `docs/02-API参考.md`
- Modify: `docs/06-高级主题.md`
- Modify: `docs/07-常见问题.md`
- Modify: remaining `test/*.cc` files returned by repository search
- Modify: remaining `examples/include/*.cpp` files returned by repository search
- Modify: remaining `benchmark/*.cc` files returned by repository search

**Step 1: Search for the remaining old surface**

Run: `rg -n "\\bCoroutine\\b|scheduleCoroutine\\(|\\.wait\\(\\)" galay-http galay-kernel test examples benchmark docs -g '!build*'`
Expected: a finite migration list that no longer includes already converted core surfaces.

**Step 2: Convert remaining repository references**

Update all remaining repository-owned code and docs to the `Task<void>` surface or remove obsolete historical examples.

**Step 3: Re-run the search**

Run: `rg -n "\\bCoroutine\\b|scheduleCoroutine\\(" galay-http galay-kernel test examples benchmark docs -g '!build*'`
Expected: no active repository usage remains outside historical plan files or external archived benchmark artifacts.

**Step 4: Build all tests, examples, and benchmarks**

Run: `cmake --build build-ssl --parallel 4`
Expected: full build succeeds.

**Step 5: Commit**

```bash
git add galay-http/module/ModulePrelude.hpp docs/01-架构设计.md docs/02-API参考.md docs/06-高级主题.md docs/07-常见问题.md test examples benchmark
git commit -m "docs: 清理 Coroutine 残留并统一 Task 用法"
```

### Task 6: Run full verification, commit the implementation version, and prepare performance comparison

**Files:**
- Modify: verification artifacts only if needed

**Step 1: Run the full test suite**

Run: `ctest --test-dir build-ssl --output-on-failure`
Expected: all tests PASS.

**Step 2: Run all examples**

Run the repository example verification script or the same command set currently used in the workspace for example validation.
Expected: all examples PASS.

**Step 3: Run all benchmarks**

Run the repository benchmark smoke suite used in the workspace.
Expected: all benchmarks PASS.

**Step 4: Verify the repository is clean and record the result**

Run:

- `git status --short`
- `rg -n "\\bCoroutine\\b|scheduleCoroutine\\(" galay-http galay-kernel test examples benchmark docs -g '!build*'`

Expected:

- only intentional modified files remain before the final commit
- no active repository `Coroutine` usage remains

**Step 5: Commit the verified implementation**

```bash
git add .
git commit -m "refactor: 全面移除 galay-http Coroutine 兼容层"
```

After this commit, use the same verified environment to compare performance against the local Go/Rust framework baselines.
