# H2c Stream Pool Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Reuse `Http2Stream` objects on the server active-connection hot path so non-SSL `h2c` avoids per-request stream allocation and teardown cost.

**Architecture:** Add an internal `Http2StreamPool` with custom-deleter return-to-pool semantics, implement a strict `Http2Stream::resetForReuse(stream_id)` reset path, and route `server + active-conn` stream creation through the pool while keeping public APIs unchanged.

**Tech Stack:** C++23, `galay-http/kernel/http2/Http2Stream.h`, `galay-http/kernel/http2/Http2StreamManager.h`, active-connection tests, `benchmark/B3-H2cServer.cc`.

---

### Task 1: Lock stream reuse semantics with a failing test

**Files:**
- Add: `test/T56-h2_stream_pool.cc`
- Modify: `galay-http/kernel/http2/Http2Stream.h`

**Step 1: Write the failing test**

Require:
- internal pool surface can acquire a stream by ID
- releasing the last reference returns the stream to the pool
- reacquire reuses the same object address but resets all request/response/event/frame-queue state

**Step 2: Run the test to verify RED**

Run:

```bash
cmake -S . -B build
cmake --build build --target T56-h2_stream_pool -j4
```

Expected: FAIL because `Http2StreamPool` and reset surface do not exist yet.

**Step 3: Implement the minimal pool and reset surface**

Implement:
- `Http2Request::clear()` / `Http2Response::clear()` if needed
- `Http2Stream::resetForReuse(stream_id)`
- internal `Http2StreamPool`

**Step 4: Run the test to verify GREEN**

Run:

```bash
./build/test/T56-h2_stream_pool
```

Expected: PASS.

### Task 2: Route server active stream creation through the pool

**Files:**
- Modify: `galay-http/kernel/http2/Http2Conn.h`
- Modify: `galay-http/kernel/http2/Http2StreamManager.h`
- Test: `test/T43-h2_active_conn_preferred.cc`
- Test: `test/T46-h2_active_conn_retire.cc`
- Test: `test/T56-h2_stream_pool.cc`

**Step 1: Extend the failing test or compile contract**

Require:
- `server + active-conn` stream creation uses the pool
- retired streams leave the connection map but remain reusable after final reference release
- client mode and non-active mode still use the existing path

**Step 2: Run focused tests to verify RED**

Run:

```bash
cmake --build build --target T43-h2_active_conn_preferred T46-h2_active_conn_retire T56-h2_stream_pool -j4
./build/test/T56-h2_stream_pool
```

Expected: FAIL until manager creation path uses the pool.

**Step 3: Implement the minimal manager integration**

Implement:
- connection stream insertion path for externally acquired streams, if needed
- manager-owned pool for active server mode
- pooled create path guarded to the target hot path only

**Step 4: Run focused tests to verify GREEN**

Run:

```bash
./build/test/T43-h2_active_conn_preferred
./build/test/T46-h2_active_conn_retire
./build/test/T56-h2_stream_pool
```

Expected: PASS.

### Task 3: Verify the active `h2c` regression set

**Files:**
- No additional source files

**Step 1: Build the targeted set**

Run:

```bash
cmake --build build --target \
  T42-h2_active_conn_api \
  T43-h2_active_conn_preferred \
  T46-h2_active_conn_retire \
  T49-h2_outbound_segments \
  T52-h2_chunked_request_body \
  T53-h2_active_batch_flush \
  T54-h2c_server_fast_path \
  T55-h2_active_batch_mailbox \
  T56-h2_stream_pool -j4
```

Expected: build succeeds.

**Step 2: Run the targeted tests**

Run:

```bash
./build/test/T42-h2_active_conn_api
./build/test/T43-h2_active_conn_preferred
./build/test/T46-h2_active_conn_retire
./build/test/T49-h2_outbound_segments
./build/test/T52-h2_chunked_request_body
./build/test/T53-h2_active_batch_flush
./build/test/T54-h2c_server_fast_path
./build/test/T55-h2_active_batch_mailbox
./build/test/T56-h2_stream_pool
```

Expected: PASS.

### Task 4: Re-run the strict fairness `h2c` benchmark

**Files:**
- Output only: `benchmark/results/<timestamp>-h2c-stream-pool/`

**Step 1: Rebuild benchmark target**

Run:

```bash
cmake --build build-ssl-nolog --target B3-H2cServer -j4
```

Expected: PASS.

**Step 2: Run fairness benchmark**

Use:
- Galay `io=4`
- Galay `compute=0`
- Rust `TOKIO_WORKER_THREADS=4`
- `140` conns
- `8s`
- `128B`
- compare only with `benchmark/results/20260308-110435-strict-runtime-fairness/aggregate.csv`

Expected: fresh result directory with `aggregate.csv`, logs, and environment snapshot.

**Step 3: Report measured delta**

Report:
- Galay `h2c` rps
- Rust `h2c` rps from same shape
- remaining gap to Rust
- whether stream pooling materially moves the needle
