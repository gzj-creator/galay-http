# HTTP/2 Chunked Request Body Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace eager HTTP/2 request-body string aggregation with a chunk-first body model, merge active-connection dispatch per reader batch, and update the `h2c` echo benchmark to echo chunks directly.

**Architecture:** Introduce an owned request-body chunk container on `Http2Request`, move DATA payload strings directly into it, batch-flush active-stream notifications once per reader batch, then add move-based chunk send helpers so `B3-H2cServer` can echo request chunks without coalescing.

**Tech Stack:** `galay-http/kernel/http2/Http2Stream.h`, `galay-http/kernel/http2/Http2StreamManager.h`, `galay-http/protoc/http2/Http2Frame.h`, `benchmark/B3-H2cServer.cc`, focused HTTP/2 tests in `test/`.

---

### Task 1: Lock the new chunk-body request API with failing tests

**Files:**
- Modify: `test/T42-h2_active_conn_api.cc`
- Add: `test/T52-h2_chunked_request_body.cc`

**Step 1: Write the failing test**

Require:
- request body exposes chunk-first APIs (`bodySize`, `bodyChunkCount`, `takeBodyChunks`, explicit coalescing helper)
- `request.body` string-centric assumptions are gone from the HTTP/2 active-conn surface
- claiming chunks empties the body container

**Step 2: Verify RED**

Run:

```bash
cmake --build build --target T42-h2_active_conn_api T52-h2_chunked_request_body -j4
./build/test/T52-h2_chunked_request_body
```

Expected: FAIL for missing new API/behavior.

### Task 2: Lock merged active-connection dispatch with a failing test

**Files:**
- Modify: `test/T43-h2_active_conn_preferred.cc`
- Modify: `test/T46-h2_active_conn_retire.cc`

**Step 1: Write the failing test**

Require:
- one `readFramesBatch()` worth of `HEADERS + DATA + END_STREAM` can be observed as a single active-stream delivery
- merged event bits still contain `HeadersReady | DataArrived | RequestComplete`

**Step 2: Verify RED**

Run:

```bash
cmake --build build --target T43-h2_active_conn_preferred T46-h2_active_conn_retire -j4
./build/test/T43-h2_active_conn_preferred
```

Expected: FAIL until batch-flush semantics are implemented.

### Task 3: Implement chunk-first request body storage

**Files:**
- Modify: `galay-http/kernel/http2/Http2Stream.h`
- Modify if needed: `galay-http/protoc/http2/Http2Frame.h`
- Add/modify: `test/T52-h2_chunked_request_body.cc`

**Step 1: Add the minimal body container**

Implement:
- owned chunk list + byte counter
- `append/move chunk`
- `takeBodyChunks()`
- explicit coalescing helper

**Step 2: Replace old string-centric request-body helpers**

Update the HTTP/2 request surface to use the new body type instead of eager string append.

**Step 3: Verify GREEN**

Run:

```bash
./build/test/T42-h2_active_conn_api
./build/test/T52-h2_chunked_request_body
```

Expected: PASS.

### Task 4: Implement reader-batch active dispatch and move DATA into chunks

**Files:**
- Modify: `galay-http/kernel/http2/Http2StreamManager.h`
- Modify if needed: `galay-http/protoc/http2/Http2Frame.h`
- Test: `test/T43-h2_active_conn_preferred.cc`
- Test: `test/T46-h2_active_conn_retire.cc`

**Step 1: Add deferred active-stream flush**

Change active-conn mode so `markStreamActive()` only records pending events and enqueue intent during frame processing, then flushes active streams once after the current reader batch finishes.

**Step 2: Move DATA payloads into request chunks**

On the server-side request path, move `Http2DataFrame` payload storage into the request body container instead of appending by copy.

**Step 3: Verify GREEN**

Run:

```bash
./build/test/T43-h2_active_conn_preferred
./build/test/T46-h2_active_conn_retire
```

Expected: PASS.

### Task 5: Add chunk send helpers and switch `B3-H2cServer`

**Files:**
- Modify: `galay-http/kernel/http2/Http2Stream.h`
- Modify: `benchmark/B3-H2cServer.cc`
- Test: `test/T33-h2_stream_frame_api.cc`
- Test: `test/T48-non_ssl_kernel_surface.cc`

**Step 1: Write/extend the failing test**

Require:
- move-based `sendDataChunks(...)` / `replyDataChunks(...)` style helper exists
- benchmark handler can claim request chunks and echo them without coalescing

**Step 2: Implement the minimal send path**

Queue one DATA packet per chunk, preserving total byte count and `END_STREAM` on the last chunk.

**Step 3: Verify GREEN**

Run:

```bash
./build/test/T33-h2_stream_frame_api
./build/test/T48-non_ssl_kernel_surface
```

Expected: PASS.

### Task 6: Run the targeted regression and strict `h2c` benchmark

**Files:**
- Output only: `benchmark/results/<timestamp>-h2c-chunked-request-body/`

**Step 1: Run targeted tests**

At minimum:
- `T33-h2_stream_frame_api`
- `T42-h2_active_conn_api`
- `T43-h2_active_conn_preferred`
- `T46-h2_active_conn_retire`
- `T48-non_ssl_kernel_surface`
- `T52-h2_chunked_request_body`

**Step 2: Rebuild benchmark binary**

Run:

```bash
cmake --build build-ssl-nolog --target B3-H2cServer -j4
```

**Step 3: Re-run strict fairness `h2c`**

Use the same fairness setup:
- Galay `io=4 compute=0`
- Rust `TOKIO_WORKER_THREADS=4`
- `140` conns
- `8s`
- `128B` payload

**Step 4: Compare against the strict baseline**

Compare against:
- `benchmark/results/20260308-110435-strict-runtime-fairness/aggregate.csv`

Report whether the new chunk-body path materially closes the remaining non-SSL gap.
