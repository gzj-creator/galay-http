# H2c Direct Active-Batch Handoff Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Remove per-stream active-connection handoff overhead on the non-SSL server HTTP/2 hot path by delivering ready streams to the active handler in direct batches instead of pushing them one-by-one through `UnsafeChannel<Http2Stream::ptr>`.

**Architecture:** Replace the current active-stream channel handoff with a lightweight per-connection batch mailbox that stores `std::vector<Http2Stream::ptr>` batches produced by `flushActiveStreams()`. Keep the public `Http2ConnContext::getActiveStreams(max_count)` API unchanged, preserve existing merged event semantics, and keep close/drain behavior compatible with current active-connection shutdown flow.

**Tech Stack:** C++23, `galay-http/kernel/http2/Http2StreamManager.h`, existing active-connection tests, `benchmark/B3-H2cServer.cc` strict fairness `h2c` benchmark.

---

### Task 1: Lock the direct batch handoff contract with a failing test

**Files:**
- Add: `test/T55-h2_active_batch_mailbox.cc`
- Test: `test/T42-h2_active_conn_api.cc`
- Test: `test/T53-h2_active_batch_flush.cc`

**Step 1: Write the failing test**

Require:

- an internal active-stream mailbox surface that accepts a whole `std::vector<Http2Stream::ptr>` batch
- `Http2ConnContext::getActiveStreams(max_count)` can consume that mailbox without using the legacy per-stream channel path
- mailbox close still causes `getActiveStreams()` to end cleanly with `std::nullopt`

**Step 2: Run test to verify it fails**

Run:

```bash
cmake --build build --target T55-h2_active_batch_mailbox -j4
```

Expected: FAIL because the direct batch mailbox surface does not exist yet.

**Step 3: Write the minimal implementation**

Implement:

- internal active batch mailbox type
- awaitable receive surface for `Http2ConnContext`
- minimal close/reset behavior needed by active handler lifecycle

**Step 4: Run test to verify it passes**

Run:

```bash
./build/test/T55-h2_active_batch_mailbox
```

Expected: PASS.

### Task 2: Route active-stream flush through the mailbox

**Files:**
- Modify: `galay-http/kernel/http2/Http2StreamManager.h`
- Test: `test/T53-h2_active_batch_flush.cc`
- Test: `test/T55-h2_active_batch_mailbox.cc`

**Step 1: Extend the failing test**

Require:

- `flushActiveStreams()` sends one direct batch into the mailbox instead of one channel send per stream
- merged stream event bits remain unchanged
- shutdown closes the mailbox exactly once and does not hang the active handler

**Step 2: Run tests to verify RED**

Run:

```bash
cmake --build build --target T53-h2_active_batch_flush T55-h2_active_batch_mailbox -j4
./build/test/T55-h2_active_batch_mailbox
```

Expected: FAIL until `Http2StreamManager` stops using `m_active_stream_channel`.

**Step 3: Write the minimal implementation**

Implement:

- manager-owned direct batch mailbox
- `Http2ConnContext` construction from that mailbox
- `flushActiveStreams()` batch swap/send path
- active handler shutdown path that closes the mailbox instead of sending a null stream sentinel

**Step 4: Run tests to verify GREEN**

Run:

```bash
./build/test/T53-h2_active_batch_flush
./build/test/T55-h2_active_batch_mailbox
```

Expected: PASS.

### Task 3: Verify the active-connection regression set

**Files:**
- No additional source files

**Step 1: Build the focused regression set**

Run:

```bash
cmake --build build --target \
  T42-h2_active_conn_api \
  T43-h2_active_conn_preferred \
  T46-h2_active_conn_retire \
  T52-h2_chunked_request_body \
  T53-h2_active_batch_flush \
  T54-h2c_server_fast_path \
  T55-h2_active_batch_mailbox -j4
```

Expected: build succeeds.

**Step 2: Run the focused regression tests**

Run:

```bash
./build/test/T42-h2_active_conn_api
./build/test/T43-h2_active_conn_preferred
./build/test/T46-h2_active_conn_retire
./build/test/T52-h2_chunked_request_body
./build/test/T53-h2_active_batch_flush
./build/test/T54-h2c_server_fast_path
./build/test/T55-h2_active_batch_mailbox
```

Expected: PASS.

### Task 4: Re-run the strict fairness `h2c` benchmark

**Files:**
- Output only: `benchmark/results/<timestamp>-h2c-direct-batch-handoff/`

**Step 1: Rebuild the benchmark target**

Run:

```bash
cmake --build build-ssl-nolog --target B3-H2cServer -j4
```

Expected: PASS.

**Step 2: Run the fairness benchmark**

Use:

- Galay `io=4`
- Galay `compute=0`
- Rust `TOKIO_WORKER_THREADS=4`
- `140` conns
- `8s`
- `128B`
- compare only against `benchmark/results/20260308-110435-strict-runtime-fairness/aggregate.csv`

Expected: a fresh result directory with `aggregate.csv`, server logs, and run logs.

**Step 3: Report the measured delta**

Report:

- Galay `h2c` rps
- Rust `h2c` rps from the same run shape
- remaining percentage gap
- whether direct batch handoff is enough, or whether the next cut should be stream lifecycle reuse/pooling
