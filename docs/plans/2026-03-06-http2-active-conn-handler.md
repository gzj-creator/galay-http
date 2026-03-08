# HTTP/2 Active Connection Handler Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add an active-connection HTTP/2 handler API that coexists with the current per-stream handler and is preferred when configured.

**Architecture:** Keep `readerLoop` as the only frame ingestion path, but add an `active stream` queue owned by `Http2StreamManager`. In active-connection mode, the reader marks streams active after state aggregation, and a single connection-level user coroutine pulls batches through `getActiveStreams()` instead of spawning one handler per stream.

**Tech Stack:** C++23 coroutines, existing `Http2StreamManager`, `UnsafeChannel`, contract-style tests in `test/`, h2c benchmark for validation.

---

### Task 1: Lock the Public API

**Files:**
- Create: `test/T42-H2ActiveConnApi.cc`
- Modify: `galay-http/kernel/http2/Http2Server.h`
- Modify: `galay-http/kernel/http2/Http2Stream.h`

**Step 1: Write the failing test**

Add a contract test that requires:
- `H2cServerBuilder::activeConnHandler(...)`
- `H2ServerBuilder::activeConnHandler(...)`
- `Http2ConnContext::getActiveStreams(size_t)`
- `Http2Stream::takeEvents()`

**Step 2: Run test to verify it fails**

Run: `cmake --build build-test --target T42-H2ActiveConnApi -j4`
Expected: compile error for missing symbols above.

**Step 3: Write minimal implementation**

Add:
- `Http2StreamEvent` bitmask enum
- `Http2ConnContext` forward declaration / wrapper skeleton
- builder overloads for active connection handler
- `takeEvents()` skeleton on `Http2Stream`

**Step 4: Run test to verify it passes**

Run: `cmake --build build-test --target T42-H2ActiveConnApi -j4`
Expected: target builds successfully.

### Task 2: Add Stream Activation State

**Files:**
- Modify: `galay-http/kernel/http2/Http2Stream.h`
- Modify: `galay-http/kernel/http2/Http2StreamManager.h`

**Step 1: Write the failing test**

Extend `test/T42-H2ActiveConnApi.cc` to require:
- `markEvent(...)`-driven `takeEvents()` clear-on-read behavior
- `Http2ConnContext::getActiveStreams()` return type contract

**Step 2: Run test to verify it fails**

Run: `cmake --build build-test --target T42-H2ActiveConnApi -j4`
Expected: compile or assertion failure for missing event API.

**Step 3: Write minimal implementation**

Add per-stream state:
- `pending_events`
- `in_active_queue`

Add helpers:
- `markEvent(Http2StreamEvent)`
- `takeEvents()`
- `setActiveQueued(bool)` / `isActiveQueued()`

**Step 4: Run test to verify it passes**

Run: `cmake --build build-test --target T42-H2ActiveConnApi -j4`
Expected: target builds successfully.

### Task 3: Implement Active Queue Awaitable

**Files:**
- Modify: `galay-http/kernel/http2/Http2StreamManager.h`
- Modify: `galay-http/kernel/http2/Http2Server.h`

**Step 1: Write the failing test**

Add a contract test that requires:
- `Http2ConnContext::getActiveStreams()` to exist and return an awaitable
- active-connection handler type to be accepted by builders

**Step 2: Run test to verify it fails**

Run: `cmake --build build-test --target T42-H2ActiveConnApi -j4`
Expected: compile error for missing awaitable or handler storage.

**Step 3: Write minimal implementation**

Add:
- `UnsafeChannel<Http2Stream::ptr>` active queue in `Http2StreamManager`
- `markStreamActive(...)`
- `Http2ConnContext` wrapper bound to a stream manager
- `getActiveStreams()` that reads batches from active queue

**Step 4: Run test to verify it passes**

Run: `cmake --build build-test --target T42-H2ActiveConnApi -j4`
Expected: target builds successfully.

### Task 4: Route Server Startup by Handler Mode

**Files:**
- Modify: `galay-http/kernel/http2/Http2Server.h`
- Modify: `galay-http/kernel/http2/Http2StreamManager.h`

**Step 1: Write the failing test**

Add a minimal behavior test or contract assertions that both handlers can coexist and active mode is preferred when both are set.

**Step 2: Run test to verify it fails**

Run: `cmake --build build-test --target T42-H2ActiveConnApi -j4`
Expected: failing assertions or compile errors for missing overload resolution.

**Step 3: Write minimal implementation**

Add server fields:
- legacy `Http2StreamHandler`
- optional `Http2ActiveConnHandler`

Wire startup:
- if active handler exists, start manager in active mode
- otherwise keep current per-stream dispatch path unchanged

**Step 4: Run test to verify it passes**

Run: `cmake --build build-test --target T42-H2ActiveConnApi -j4`
Expected: target builds successfully.

### Task 5: Emit Active Events from Reader Dispatch

**Files:**
- Modify: `galay-http/kernel/http2/Http2StreamManager.h`

**Step 1: Write the failing test**

Add a focused unit-style contract test for event marking on:
- headers completion
- data arrival
- request completion
- reset

**Step 2: Run test to verify it fails**

Run: `cmake --build build-test --target T42-H2ActiveConnApi -j4`
Expected: missing calls or wrong event semantics.

**Step 3: Write minimal implementation**

In `dispatchStreamFrame(...)`:
- keep existing aggregation logic
- call `markStreamActive(stream, event)` after state changes
- skip frame queue dependency in active mode where possible

**Step 4: Run test to verify it passes**

Run: `cmake --build build-test --target T42-H2ActiveConnApi -j4`
Expected: target builds successfully.

### Task 6: Validate via h2c Benchmark

**Files:**
- Modify: `benchmark/B3-H2cServer.cc`

**Step 1: Write the failing test**

Switch benchmark server to use `activeConnHandler(...)` while preserving echo behavior.

**Step 2: Run benchmark/build to verify integration is incomplete**

Run: `cmake --build build-ssl-nolog --target B3-H2cServer -j4`
Expected: build failure until active mode is fully wired.

**Step 3: Write minimal implementation**

Adapt benchmark server:
- use active-connection loop
- `co_await ctx.getActiveStreams(64)`
- respond from aggregated request body

**Step 4: Run validation**

Run:
- `cmake --build build-test --target T42-H2ActiveConnApi -j4`
- `cmake --build build-ssl-nolog --target B3-H2cServer -j4`
- h2c 3-round benchmark with existing compare client

Expected:
- test target builds
- benchmark target builds
- h2c throughput is at least not worse than current `waitRequestComplete()` baseline.

### Notes

- Do not touch unrelated deleted files under `test/files/` or `test/renumber.sh`.
- Remove or replace the temporary `stream_frame_queue_enabled` tuning if active mode supersedes it.
- First implementation scope is server-side active mode; client-side active pulling stays out of scope.
