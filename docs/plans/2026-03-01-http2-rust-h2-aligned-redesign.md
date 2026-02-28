# HTTP/2 Rust-h2 Aligned Redesign Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the entire HTTP/2 implementation with a Rust `h2`-style connection-loop + frame-dispatch architecture and frame-first stream APIs.

**Architecture:** Build a single connection protocol core that owns frame IO, state transitions, flow-control, timers, and outbound scheduling. Streams become state/event endpoints and never write sockets directly. Public APIs switch to `getFrame` + `replyHeader/replyData/replyRst`.

**Tech Stack:** C++23 coroutines, galay-kernel awaitables/scheduler/runtime, existing galay-http test/example/cmake structure.

---

**Execution rules for all tasks**

- Use `@test-driven-development` for every behavior change.
- Use `@verification-before-completion` before each completion claim.
- Keep commits small and task-scoped.
- Do not add compatibility adapters for removed HTTP/2 APIs.

### Task 1: Lock API Contract With Compile-Fail Tests

**Files:**
- Modify: `test/T25-H2cServer.cc`
- Modify: `test/T25-H2cClient.cc`
- Modify: `test/T27-H2Server.cc`
- Modify: `test/T28-H2Client.cc`

**Step 1: Write failing test edits (API contract only)**

```cpp
// Expected usage shape in tests/examples:
auto frame = co_await stream->getFrame();
co_await stream->replyHeader(Http2Headers().status(200), false);
co_await stream->replyData("ok", true);
```

**Step 2: Run compile to verify it fails**

Run: `cmake --build build --target T25-H2cServer T25-H2cClient T27-H2Server T28-H2Client --parallel 4`
Expected: FAIL because new methods are missing.

**Step 3: Commit failing-contract checkpoint**

```bash
git add test/T25-H2cServer.cc test/T25-H2cClient.cc test/T27-H2Server.cc test/T28-H2Client.cc
git commit -m "test(http2): lock frame-first stream API contract"
```

### Task 2: Rebuild HTTP/2 Error Taxonomy

**Files:**
- Modify: `galay-http/protoc/http2/Http2Error.h`
- Create: `test/T29-H2ErrorModel.cc`

**Step 1: Write failing unit test**

```cpp
// Assert explicit categories: protocol/flow-control/stream/timeout/peer-closed
```

**Step 2: Run test target to verify fail**

Run: `cmake --build build --target T29-H2ErrorModel --parallel 4`
Expected: FAIL (missing new enums/helpers).

**Step 3: Implement minimal error model**

```cpp
enum class Http2RuntimeError {
  ProtocolViolation,
  FlowControlViolation,
  StreamClosed,
  StreamReset,
  Timeout,
  PeerClosed,
  IoError
};
```

**Step 4: Rebuild and run**

Run: `cmake --build build --target T29-H2ErrorModel --parallel 4 && ./build/test/T29-H2ErrorModel`
Expected: PASS.

**Step 5: Commit**

```bash
git add galay-http/protoc/http2/Http2Error.h test/T29-H2ErrorModel.cc
git commit -m "feat(http2): introduce explicit runtime error taxonomy"
```

### Task 3: Rebuild Frame Type/Core Codec Skeleton

**Files:**
- Modify: `galay-http/protoc/http2/Http2Base.h`
- Modify: `galay-http/protoc/http2/Http2Base.cc`
- Modify: `galay-http/protoc/http2/Http2Frame.h`
- Modify: `galay-http/protoc/http2/Http2Frame.cc`
- Create: `test/T30-H2FrameCodec.cc`

**Step 1: Write failing codec tests**

```cpp
// Round-trip for HEADERS, DATA, SETTINGS, PING, GOAWAY, WINDOW_UPDATE
```

**Step 2: Verify fail**

Run: `cmake --build build --target T30-H2FrameCodec --parallel 4`
Expected: FAIL.

**Step 3: Implement minimal passing codec**

```cpp
// parse header -> instantiate frame -> parse payload
// serialize frame -> header + payload
```

**Step 4: Verify pass**

Run: `cmake --build build --target T30-H2FrameCodec --parallel 4 && ./build/test/T30-H2FrameCodec`
Expected: PASS.

**Step 5: Commit**

```bash
git add galay-http/protoc/http2/Http2Base.h galay-http/protoc/http2/Http2Base.cc galay-http/protoc/http2/Http2Frame.h galay-http/protoc/http2/Http2Frame.cc test/T30-H2FrameCodec.cc
git commit -m "feat(http2): rebuild frame codec core"
```

### Task 4: Rebuild HPACK Public Contract

**Files:**
- Modify: `galay-http/protoc/http2/Http2Hpack.h`
- Modify: `galay-http/protoc/http2/Http2Hpack.cc`
- Create: `test/T31-H2Hpack.cc`

**Step 1: Write failing tests**

```cpp
// encode/decode round-trip + dynamic table size update + header list limit behavior
```

**Step 2: Verify fail**

Run: `cmake --build build --target T31-H2Hpack --parallel 4`
Expected: FAIL.

**Step 3: Implement minimal passing behavior**

```cpp
// deterministic table update + strict bound checks
```

**Step 4: Verify pass**

Run: `cmake --build build --target T31-H2Hpack --parallel 4 && ./build/test/T31-H2Hpack`
Expected: PASS.

**Step 5: Commit**

```bash
git add galay-http/protoc/http2/Http2Hpack.h galay-http/protoc/http2/Http2Hpack.cc test/T31-H2Hpack.cc
git commit -m "feat(http2): rebuild hpack contract"
```

### Task 5: Introduce Connection Core Runtime Object

**Files:**
- Create: `galay-http/kernel/http2/Http2ConnectionCore.h`
- Create: `galay-http/kernel/http2/Http2ConnectionCore.cc`
- Modify: `galay-http/kernel/http2/Http2Conn.h`
- Create: `test/T32-H2ConnectionCoreLifecycle.cc`

**Step 1: Write failing lifecycle test**

```cpp
// start -> settings exchange -> stop
```

**Step 2: Verify fail**

Run: `cmake --build build --target T32-H2ConnectionCoreLifecycle --parallel 4`
Expected: FAIL.

**Step 3: Implement minimal core loop shell**

```cpp
Coroutine run() {
  while (!closing_) {
    co_await readOne();
    applyOne();
    flushOutbound();
    checkTimers();
  }
}
```

**Step 4: Verify pass**

Run: `cmake --build build --target T32-H2ConnectionCoreLifecycle --parallel 4 && ./build/test/T32-H2ConnectionCoreLifecycle`
Expected: PASS.

**Step 5: Commit**

```bash
git add galay-http/kernel/http2/Http2ConnectionCore.h galay-http/kernel/http2/Http2ConnectionCore.cc galay-http/kernel/http2/Http2Conn.h test/T32-H2ConnectionCoreLifecycle.cc
git commit -m "feat(http2): add connection core loop skeleton"
```

### Task 6: Replace Stream API With Frame-First Surface

**Files:**
- Modify: `galay-http/kernel/http2/Http2Stream.h`
- Create: `test/T33-H2StreamFrameApi.cc`

**Step 1: Write failing stream API tests**

```cpp
// methods: getFrame/replyHeader/replyData/replyRst
// removed: readRequest/readResponse
```

**Step 2: Verify fail**

Run: `cmake --build build --target T33-H2StreamFrameApi --parallel 4`
Expected: FAIL.

**Step 3: Implement minimal API surface**

```cpp
auto getFrame();
auto replyHeader(const std::vector<Http2HeaderField>&, bool end_stream);
auto replyData(const std::string&, bool end_stream);
auto replyRst(Http2ErrorCode);
```

**Step 4: Verify pass**

Run: `cmake --build build --target T33-H2StreamFrameApi --parallel 4 && ./build/test/T33-H2StreamFrameApi`
Expected: PASS.

**Step 5: Commit**

```bash
git add galay-http/kernel/http2/Http2Stream.h test/T33-H2StreamFrameApi.cc
git commit -m "feat(http2): switch stream to frame-first API"
```

### Task 7: Implement Deterministic Inbound Frame Dispatcher

**Files:**
- Create: `galay-http/kernel/http2/Http2FrameDispatcher.h`
- Create: `galay-http/kernel/http2/Http2FrameDispatcher.cc`
- Modify: `galay-http/kernel/http2/Http2ConnectionCore.cc`
- Create: `test/T34-H2DispatcherStateMachine.cc`

**Step 1: Write failing state machine tests**

```cpp
// HEADERS/CONTINUATION ordering, stream-id checks, rst/goaway behavior
```

**Step 2: Verify fail**

Run: `cmake --build build --target T34-H2DispatcherStateMachine --parallel 4`
Expected: FAIL.

**Step 3: Implement minimal dispatcher**

```cpp
DispatchResult dispatch(const Http2Frame& frame, ConnectionState& conn, StreamTable& streams);
```

**Step 4: Verify pass**

Run: `cmake --build build --target T34-H2DispatcherStateMachine --parallel 4 && ./build/test/T34-H2DispatcherStateMachine`
Expected: PASS.

**Step 5: Commit**

```bash
git add galay-http/kernel/http2/Http2FrameDispatcher.h galay-http/kernel/http2/Http2FrameDispatcher.cc galay-http/kernel/http2/Http2ConnectionCore.cc test/T34-H2DispatcherStateMachine.cc
git commit -m "feat(http2): add deterministic inbound frame dispatcher"
```

### Task 8: Implement Outbound Scheduler + Flow Control

**Files:**
- Create: `galay-http/kernel/http2/Http2OutboundScheduler.h`
- Create: `galay-http/kernel/http2/Http2OutboundScheduler.cc`
- Modify: `galay-http/kernel/http2/Http2ConnectionCore.cc`
- Create: `test/T35-H2FlowControl.cc`

**Step 1: Write failing flow-control tests**

```cpp
// conn window + stream window gating + window update recovery + max_frame_size split
```

**Step 2: Verify fail**

Run: `cmake --build build --target T35-H2FlowControl --parallel 4`
Expected: FAIL.

**Step 3: Implement minimal scheduler behavior**

```cpp
std::vector<Http2Frame::uptr> pickSendableFrames(...);
```

**Step 4: Verify pass**

Run: `cmake --build build --target T35-H2FlowControl --parallel 4 && ./build/test/T35-H2FlowControl`
Expected: PASS.

**Step 5: Commit**

```bash
git add galay-http/kernel/http2/Http2OutboundScheduler.h galay-http/kernel/http2/Http2OutboundScheduler.cc galay-http/kernel/http2/Http2ConnectionCore.cc test/T35-H2FlowControl.cc
git commit -m "feat(http2): add outbound scheduler with flow control"
```

### Task 9: Integrate Timer Rules Into Connection Core

**Files:**
- Modify: `galay-http/kernel/http2/Http2ConnectionCore.h`
- Modify: `galay-http/kernel/http2/Http2ConnectionCore.cc`
- Create: `test/T36-H2TimerBehavior.cc`

**Step 1: Write failing timer tests**

```cpp
// settings-ack timeout, ping-ack timeout, graceful shutdown deadline
```

**Step 2: Verify fail**

Run: `cmake --build build --target T36-H2TimerBehavior --parallel 4`
Expected: FAIL.

**Step 3: Implement minimal timer checks**

```cpp
void checkTimers(std::chrono::steady_clock::time_point now);
```

**Step 4: Verify pass**

Run: `cmake --build build --target T36-H2TimerBehavior --parallel 4 && ./build/test/T36-H2TimerBehavior`
Expected: PASS.

**Step 5: Commit**

```bash
git add galay-http/kernel/http2/Http2ConnectionCore.h galay-http/kernel/http2/Http2ConnectionCore.cc test/T36-H2TimerBehavior.cc
git commit -m "feat(http2): integrate timer behavior in connection core"
```

### Task 10: Rebuild H2c Client/Server On New Core

**Files:**
- Modify: `galay-http/kernel/http2/H2cClient.h`
- Modify: `galay-http/kernel/http2/Http2Server.h`
- Modify: `test/T25-H2cServer.cc`
- Modify: `test/T25-H2cClient.cc`

**Step 1: Write failing integration assertions**

```cpp
// use getFrame + replyHeader/replyData on h2c paths
```

**Step 2: Verify fail**

Run: `cmake --build build --target T25-H2cServer T25-H2cClient --parallel 4`
Expected: FAIL.

**Step 3: Implement minimal integration**

```cpp
// wire connection core start/stop + stream command bridge
```

**Step 4: Verify pass (build + smoke run)**

Run:
- `cmake --build build --target T25-H2cServer T25-H2cClient --parallel 4`
- `./build/test/T25-H2cServer 9080` (terminal A)
- `./build/test/T25-H2cClient 127.0.0.1 9080 5` (terminal B)
Expected: successful exchanges.

**Step 5: Commit**

```bash
git add galay-http/kernel/http2/H2cClient.h galay-http/kernel/http2/Http2Server.h test/T25-H2cServer.cc test/T25-H2cClient.cc
git commit -m "feat(http2): rebuild h2c client/server on new core"
```

### Task 11: Rebuild H2 TLS Client/Server On New Core

**Files:**
- Modify: `galay-http/kernel/http2/H2Client.h`
- Modify: `galay-http/kernel/http2/Http2Server.h`
- Modify: `test/T27-H2Server.cc`
- Modify: `test/T28-H2Client.cc`

**Step 1: Write failing TLS integration assertions**

```cpp
// frame-first API over ALPN h2
```

**Step 2: Verify fail**

Run: `cmake --build build --target T27-H2Server T28-H2Client --parallel 4`
Expected: FAIL.

**Step 3: Implement minimal integration**

```cpp
// TLS path shares same connection core and stream API
```

**Step 4: Verify pass (build + smoke run)**

Run:
- `cmake --build build --target T27-H2Server T28-H2Client --parallel 4`
- `./build/test/T27-H2Server 9443 test.crt test.key` (terminal A)
- `./build/test/T28-H2Client 127.0.0.1 9443 5` (terminal B)
Expected: successful ALPN h2 requests.

**Step 5: Commit**

```bash
git add galay-http/kernel/http2/H2Client.h galay-http/kernel/http2/Http2Server.h test/T27-H2Server.cc test/T28-H2Client.cc
git commit -m "feat(http2): rebuild h2 tls client/server on new core"
```

### Task 12: Remove Legacy Paths And Update Module/Docs

**Files:**
- Modify: `galay-http/module/galay.http2.cppm`
- Modify: `docs/01-架构设计.md`
- Modify: `docs/02-API参考.md`
- Modify: `docs/03-使用指南.md`
- Modify: `docs/04-示例代码.md`
- Modify: `docs/06-高级主题.md`
- Modify: `docs/07-常见问题.md`
- Modify: `README.md`
- (As needed) modify/delete deprecated internals under `galay-http/kernel/http2/*`

**Step 1: Write failing docs/API consistency checks**

```bash
rg -n "readRequest\(|readResponse\(|replyAndWait\(" docs README.md galay-http/kernel/http2
```

Expected (pre-fix): stale references found.

**Step 2: Remove stale references and deprecated API surfaces**

```cpp
// Ensure public API docs and module exports only expose frame-first methods
```

**Step 3: Verify clean state**

Run:
- `rg -n "readRequest\(|readResponse\(|replyAndWait\(" docs README.md galay-http/kernel/http2`
- `cmake --build build --parallel 4`
Expected: no stale API matches and successful build.

**Step 4: Commit**

```bash
git add README.md docs/01-架构设计.md docs/02-API参考.md docs/03-使用指南.md docs/04-示例代码.md docs/06-高级主题.md docs/07-常见问题.md galay-http/module/galay.http2.cppm galay-http/kernel/http2
git commit -m "refactor(http2): remove legacy paths and finalize frame-first public surface"
```

### Task 13: Final Verification Gate

**Files:**
- No new files required

**Step 1: Run full target verification**

Run:
- `cmake --build build --parallel 4`
- `cmake --build build --target T25-H2cServer T25-H2cClient T27-H2Server T28-H2Client --parallel 4`

**Step 2: Run smoke tests**

Run h2c and h2 manual exchange (same commands as Task 10/11).

**Step 3: Run static API grep checks**

Run:
- `rg -n "readRequest\(|readResponse\(" galay-http/kernel/http2 docs README.md`
- `rg -n "getFrame\(|replyHeader\(|replyData\(" galay-http/kernel/http2 test examples docs`

**Step 4: Final commit (if any leftover fixups)**

```bash
git add -A
git commit -m "chore(http2): final verification fixups"
```
