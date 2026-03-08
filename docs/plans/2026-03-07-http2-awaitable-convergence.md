# HTTP/2 Awaitable Convergence Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Converge HTTP/2 shutdown and TLS connect entrypoints onto direct awaitables without changing observable behavior.

**Architecture:** First replace wrapper coroutine `.wait()` shutdown paths with direct awaitables rooted in `Http2StreamManager`, then migrate TLS connect only if the direct awaitable path remains behavior-equivalent to the current `connectImpl()` sequence. Prefer scheduler-driven direct awaitables over helper coroutines, but do not trade correctness for mechanical flattening.

**Tech Stack:** C++23 coroutines, galay-kernel awaitables, HTTP/2 stream manager, TCP/TLS clients.

---

### Task 1: Lock shutdown/connect surface with tests

**Files:**
- Modify: `test/T45-H2AwaitableSurface.cc`
- Test: `test/T44-H2cClientShutdown.cc`
- Test: `test/T28-H2Client.cc`

**Step 1: Write the failing test**

- Add compile-time assertions for the public awaitable surface that should become direct.
- Keep the runtime H2c shutdown regression and TLS connect regression as behavioral guards.

**Step 2: Run test to verify it fails**

Run: `cmake --build build-test --target T45-H2AwaitableSurface -j4`
Expected: compile failure before the direct awaitable surface is in place.

**Step 3: Write minimal implementation**

- Change one public entrypoint at a time.
- Keep runtime behavior identical to existing tests.

**Step 4: Run test to verify it passes**

Run: `cmake --build build-test --target T45-H2AwaitableSurface -j4`
Expected: build succeeds.

### Task 2: Converge shutdown onto direct awaitables

**Files:**
- Modify: `galay-http/kernel/http2/Http2StreamManager.h`
- Modify: `galay-http/kernel/http2/H2cClient.h`
- Modify: `galay-http/kernel/http2/H2Client.h`
- Test: `test/T44-H2cClientShutdown.cc`

**Step 1: Write the failing test**

- Assert the public shutdown/close entrypoints no longer route through wrapper coroutine types where practical.
- Reuse `T44` to hold runtime semantics.

**Step 2: Run test to verify it fails**

Run: `cmake --build build-test --target T44-H2cClientShutdown T45-H2AwaitableSurface -j4`
Expected: compile or behavior failure before implementation.

**Step 3: Write minimal implementation**

- Make `Http2StreamManager` expose a direct shutdown awaitable.
- Make client shutdown/close awaitables delegate to that direct awaitable instead of helper coroutine `.wait()`.

**Step 4: Run test to verify it passes**

Run: `./build-test/test/T44-H2cClientShutdown`
Expected: PASS.

### Task 3: Migrate TLS connect only if behavior stays equivalent

**Files:**
- Modify: `galay-http/kernel/http2/H2Client.h`
- Test: `test/T28-H2Client.cc`

**Step 1: Write the failing test**

- Extend `T45` to require the direct TLS connect awaitable surface only after the runtime path is shown to be equivalent.

**Step 2: Run test to verify it fails**

Run: `cmake --build build-ssl --target T45-H2AwaitableSurface -j4`
Expected: compile failure before the API switch.

**Step 3: Write minimal implementation**

- Preserve the exact sequence of socket setup, connect, handshake, ALPN validation, preface send, and manager bootstrap.
- Reject any shortcut that changes the TLS/H2 handshake ordering.

**Step 4: Run test to verify it passes**

Run: `./build-ssl/test/T28-H2Client localhost 19443 5`
Expected: `Success: 5`.

### Task 4: Verify the whole regression set

**Files:**
- No source changes

**Step 1: Run targeted build/test verification**

Run: `cmake --build build-test --target T41-H2CloseTcpTeardown T42-H2ActiveConnApi T43-H2ActiveConnPreferred T44-H2cClientShutdown T45-H2AwaitableSurface -j4`

**Step 2: Run runtime regressions**

Run:
- `./build-test/test/T41-H2CloseTcpTeardown`
- `./build-test/test/T42-H2ActiveConnApi`
- `./build-test/test/T43-H2ActiveConnPreferred`
- `./build-test/test/T44-H2cClientShutdown`

**Step 3: Run TLS regression**

Run:
- `cmake --build build-ssl --target T27-H2Server T28-H2Client T45-H2AwaitableSurface -j4`
- Start `T27-H2Server`, then run `T28-H2Client localhost 19443 5`

**Step 4: Run benchmark target build**

Run: `cmake --build build-ssl-nolog --target B3-H2cServer -j4`
Expected: build succeeds.
