# Non-SSL Performance Convergence Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Re-baseline galay-http on the updated `galay-kernel`, then close the remaining non-SSL `h2c`, `http`, and `ws` performance gaps so service-side throughput reaches at least 90% of the comparable Go/Rust baseline while client-side paths do not become the bottleneck.

**Architecture:** Work in protocol order `h2c -> http -> ws`. For each protocol, first lock correctness with focused tests around the hot-path helper or behavior being changed, then apply the smallest code change that removes avoidable awaitable, wakeup, descriptor, or buffer churn. Re-run the benchmark after every phase and only touch shared runtime-facing surfaces if the updated-kernel baseline proves a common bottleneck still dominates.

**Tech Stack:** C++23 coroutines, `galay-kernel` async sockets/schedulers, HTTP/1.1, WebSocket, HTTP/2 cleartext, benchmark targets under `benchmark/`, targeted tests under `test/`.

---

> Commit steps are intentionally omitted from every task. Only create commits if the user explicitly asks for them.

### Task 1: Capture a trusted non-SSL baseline on the updated kernel

**Files:**
- Reference: `benchmark/compare/protocols/run_compare.sh`
- Record: `benchmark/results/<timestamp>-nonssl-kernel-rebaseline/`

**Step 1: Rebuild the non-SSL benchmark targets**

Run: `cmake --build build-ssl-nolog --target B1-HttpServer B2-HttpClient B3-H2cServer B4-H2cClient B5-WebsocketServer B6-WebsocketClient -j4`
Expected: all six benchmark targets build successfully against the updated `galay-kernel`.

**Step 2: Run the existing compare harness and keep the non-SSL rows**

Run: `GALAY_BUILD_DIR=$PWD/build-ssl-nolog BENCH_THREADS=4 benchmark/compare/protocols/run_compare.sh`
Expected: a new `benchmark/results/<timestamp>-galay-go-rust-http-proto-compare/` directory appears with `metrics.csv`, `summary.md`, server logs, and sample/flame outputs.

**Step 3: Extract the current `http`, `ws`, and `h2c` baselines**

Record the `galay`, `go`, and `rust` rows for `http`, `ws`, and `h2c`, plus the hottest stack from the generated sample/flame outputs for each protocol.

**Step 4: Rank the next optimization target**

Stop and rank the remaining non-SSL gaps. If the new baseline shows `h2c` is still the largest service-side gap, continue with Task 2. If another protocol has become the dominant gap, adjust the execution order before touching code.

### Task 2: Lock the `h2c` helper surfaces before further optimization

**Files:**
- Modify: `test/T42-H2ActiveConnApi.cc`
- Modify: `test/T43-H2ActiveConnPreferred.cc`
- Modify: `test/T44-H2cClientShutdown.cc`
- Modify: `test/T45-H2AwaitableSurface.cc`
- Modify: `test/T46-H2ActiveConnRetire.cc`
- Modify: `test/T47-IoVecBorrowCursor.cc`
- Modify: `galay-http/kernel/IoVecUtils.h`

**Step 1: Write the failing tests**

Tighten the focused guards so they require:

- stable active-connection stream retrieval semantics
- stable shutdown/close awaitable behavior
- the direct/borrowed helper surfaces needed for the current hot path
- no regression in stream retirement behavior

**Step 2: Run the targeted builds to verify the guards fail where expected**

Run: `cmake --build build-test --target T42-H2ActiveConnApi T43-H2ActiveConnPreferred T44-H2cClientShutdown T45-H2AwaitableSurface T46-H2ActiveConnRetire T47-IoVecBorrowCursor -j4`
Expected: either a compile failure or a targeted assertion failure that proves the new guard is active before the code change.

**Step 3: Write the minimal implementation**

Only add or finalize the helper surface required by those tests:

- borrowed `iovec` cursor behavior in `IoVecUtils.h`
- any direct awaitable surface already proven necessary by the existing `h2c` experiments

Do not mix benchmark-only refactors into this task.

**Step 4: Run the targeted tests to verify they pass**

Run:
- `./build-test/test/T42-H2ActiveConnApi`
- `./build-test/test/T43-H2ActiveConnPreferred`
- `./build-test/test/T44-H2cClientShutdown`
- `./build-test/test/T46-H2ActiveConnRetire`
- `./build-test/test/T47-IoVecBorrowCursor`

Expected: PASS.

### Task 3: Converge the `h2c` server hot path

**Files:**
- Modify: `galay-http/kernel/http2/Http2StreamManager.h`
- Modify: `galay-http/kernel/http2/Http2OutboundScheduler.cc`
- Modify: `galay-http/kernel/http2/Http2Stream.h`
- Test: `test/T25-H2cServer.cc`
- Test: `test/T42-H2ActiveConnApi.cc`
- Test: `test/T43-H2ActiveConnPreferred.cc`
- Test: `test/T46-H2ActiveConnRetire.cc`

**Step 1: Write or extend the failing behavioral guard**

If the existing `T42/T43/T46` coverage is not sufficient to lock the exact server-side behavior touched by the optimization, extend `T25-H2cServer.cc` or the targeted active-conn tests first so stream visibility, header/data ordering, and retirement semantics are pinned.

**Step 2: Run the server-side `h2c` regression set**

Run:
- `cmake --build build-test --target T25-H2cServer T42-H2ActiveConnApi T43-H2ActiveConnPreferred T46-H2ActiveConnRetire -j4`
- `./build-test/test/T25-H2cServer`
- `./build-test/test/T42-H2ActiveConnApi`
- `./build-test/test/T43-H2ActiveConnPreferred`
- `./build-test/test/T46-H2ActiveConnRetire`

Expected: PASS baseline before refactor.

**Step 3: Write the minimal implementation**

Optimize only the hottest server-side `h2c` path exposed by Task 1:

- remove avoidable writer-loop descriptor export/copy work
- reduce avoidable wake/defer churn in the active-connection loop
- keep frame ordering, flow-control behavior, and stream retirement unchanged

**Step 4: Run regressions and a focused `h2c` benchmark**

Run:
- `./build-test/test/T25-H2cServer`
- `./build-test/test/T42-H2ActiveConnApi`
- `./build-test/test/T43-H2ActiveConnPreferred`
- `./build-test/test/T46-H2ActiveConnRetire`
- `cmake --build build-ssl-nolog --target B3-H2cServer B4-H2cClient -j4`

Then run the same focused `h2c` benchmark shape used by the current compare harness and save the result under `benchmark/results/<timestamp>-h2c-server-convergence/`.

Expected: tests still pass and the `h2c` server result improves or at least does not regress.

### Task 4: Converge the `h2c` client hot path

**Files:**
- Modify: `galay-http/kernel/http2/H2cClient.h`
- Modify: `galay-http/kernel/http2/Http2Conn.h`
- Modify: `galay-http/kernel/http2/Http2StreamManager.h`
- Test: `test/T25-H2cClient.cc`
- Test: `test/T44-H2cClientShutdown.cc`
- Test: `test/T45-H2AwaitableSurface.cc`

**Step 1: Write the failing client-side guard**

Tighten the client-side tests so they lock:

- request send/receive behavior under the optimized path
- shutdown and close behavior
- any direct awaitable surface adopted by the client

**Step 2: Run the client-side `h2c` regression set**

Run:
- `cmake --build build-test --target T25-H2cClient T44-H2cClientShutdown T45-H2AwaitableSurface -j4`
- `./build-test/test/T25-H2cClient`
- `./build-test/test/T44-H2cClientShutdown`

Expected: PASS baseline before refactor, or an intentional failing guard that proves the new assertion is live.

**Step 3: Write the minimal implementation**

Optimize only the hottest client-side `h2c` path shown by the re-baseline:

- reduce request-side awaitable layering
- avoid redundant descriptor/buffer materialization
- preserve connect/send/shutdown ordering and observable behavior

**Step 4: Re-run client regressions and the paired `h2c` benchmark**

Run:
- `./build-test/test/T25-H2cClient`
- `./build-test/test/T44-H2cClientShutdown`
- `cmake --build build-ssl-nolog --target B3-H2cServer B4-H2cClient -j4`

Then re-run the same `h2c` benchmark shape and compare the new galay numbers against both the Task 1 baseline and the latest Go/Rust rows.

Expected: no behavior regression and no client-side bottleneck hiding server gains.

### Task 5: Converge the HTTP/1.1 fast path

**Files:**
- Modify: `galay-http/kernel/http/HttpReader.h`
- Modify: `galay-http/kernel/http/HttpWriter.h`
- Modify: `galay-http/kernel/http/HttpConn.h`
- Modify: `galay-http/protoc/http/HttpHeader.h`
- Modify: `galay-http/utils/Http1_1RequestBuilder.h`
- Test: `test/T3-ReaderWriterServer.cc`
- Test: `test/T5-HttpServer.cc`
- Test: `test/T6-HttpClientAwaitable.cc`
- Test: `test/T7-HttpClientAwaitableEdgeCases.cc`
- Test: `test/T39-HeaderFastPath.cc`
- Test: `test/T40-ClientHeaderCase.cc`

**Step 1: Write the failing HTTP guards**

Extend the HTTP tests so they pin the behavior of the exact hot path to be optimized:

- response send behavior for small echo/static responses
- header fast-path behavior and duplicate/header-case handling
- client awaitable behavior for the request path

**Step 2: Run the HTTP regression set**

Run:
- `cmake --build build-test --target T3-ReaderWriterServer T5-HttpServer T6-HttpClientAwaitable T7-HttpClientAwaitableEdgeCases T39-HeaderFastPath T40-ClientHeaderCase -j4`
- `./build-test/test/T3-ReaderWriterServer`
- `./build-test/test/T5-HttpServer`
- `./build-test/test/T6-HttpClientAwaitable`
- `./build-test/test/T7-HttpClientAwaitableEdgeCases`
- `./build-test/test/T39-HeaderFastPath`
- `./build-test/test/T40-ClientHeaderCase`

Expected: PASS baseline before refactor, or a targeted failing assertion that proves the new guard is active.

**Step 3: Write the minimal implementation**

Optimize only the hottest HTTP path surfaced by Task 1:

- reduce response-side header/default-header churn
- reduce small-response buffer assembly and send fragmentation
- avoid turning the benchmark client request path into the bottleneck

**Step 4: Run regressions and the HTTP benchmark**

Run:
- the full HTTP regression set above
- `cmake --build build-ssl-nolog --target B1-HttpServer B2-HttpClient -j4`

Then run the HTTP benchmark shape corresponding to the compare harness and save the result under `benchmark/results/<timestamp>-http-convergence/`.

Expected: all HTTP tests pass and the HTTP benchmark moves closer to the Go/Rust baseline.

### Task 6: Converge the WebSocket fast path

**Files:**
- Modify: `galay-http/kernel/websocket/WsReader.h`
- Modify: `galay-http/kernel/websocket/WsWriter.h`
- Modify: `galay-http/kernel/websocket/WsConn.h`
- Modify: `galay-http/protoc/websocket/WebSocketFrame.h`
- Modify: `galay-http/protoc/websocket/WebSocketFrame.cc`
- Test: `test/T18-WsServer.cc`
- Test: `test/T19-WsClient.cc`
- Test: `test/T20-WebsocketClient.cc`

**Step 1: Write the failing WebSocket guards**

Extend the WebSocket tests so they pin:

- upgrade-to-frame-path behavior
- small text echo behavior
- ping/pong and close semantics

**Step 2: Run the WebSocket regression set**

Run:
- `cmake --build build-test --target T18-WsServer T19-WsClient T20-WebsocketClient -j4`
- start `./build-test/test/T18-WsServer`
- run `./build-test/test/T19-WsClient localhost 8080 5`
- run `./build-test/test/T20-WebsocketClient`

Expected: PASS baseline before refactor, or a targeted failing assertion that proves the new guard is active.

**Step 3: Write the minimal implementation**

Optimize only the hottest WebSocket path surfaced by Task 1:

- reduce per-frame parse/build overhead in the small-message echo path
- reduce mask/header/payload descriptor churn
- keep ping/pong/close behavior unchanged

**Step 4: Run regressions and the WebSocket benchmark**

Run:
- the full WebSocket regression set above
- `cmake --build build-ssl-nolog --target B5-WebsocketServer B6-WebsocketClient -j4`

Then run the WebSocket benchmark shape corresponding to the compare harness and save the result under `benchmark/results/<timestamp>-ws-convergence/`.

Expected: all WebSocket tests pass and the WebSocket benchmark moves closer to the Go/Rust baseline.

### Task 7: Run the final non-SSL verification and compare against acceptance targets

**Files:**
- Record: `benchmark/results/<timestamp>-nonssl-convergence/`
- Reference: `docs/plans/2026-03-07-nonssl-perf-convergence-design.md`

**Step 1: Rebuild all targeted tests and non-SSL benchmarks**

Run: `cmake --build build-test --target T3-ReaderWriterServer T5-HttpServer T6-HttpClientAwaitable T7-HttpClientAwaitableEdgeCases T18-WsServer T19-WsClient T20-WebsocketClient T25-H2cServer T25-H2cClient T39-HeaderFastPath T40-ClientHeaderCase T42-H2ActiveConnApi T43-H2ActiveConnPreferred T44-H2cClientShutdown T45-H2AwaitableSurface T46-H2ActiveConnRetire T47-IoVecBorrowCursor -j4 && cmake --build build-ssl-nolog --target B1-HttpServer B2-HttpClient B3-H2cServer B4-H2cClient B5-WebsocketServer B6-WebsocketClient -j4`
Expected: build succeeds.

**Step 2: Re-run the compare harness**

Run: `GALAY_BUILD_DIR=$PWD/build-ssl-nolog BENCH_THREADS=4 benchmark/compare/protocols/run_compare.sh`
Expected: a fresh result directory appears with comparable galay/go/rust metrics.

**Step 3: Compare only the non-SSL rows against Task 1**

Confirm the final `http`, `ws`, and `h2c` rows:

- meet or exceed 90% of the comparable Go/Rust baseline for the chosen run shape
- do not show a clearly material average-latency regression
- are not obviously limited by the galay benchmark client path

**Step 4: Summarize the remaining gaps**

Write down which non-SSL protocol still has the largest residual gap and whether it now points to a protocol-local hotspot or a genuinely shared runtime-facing bottleneck.
