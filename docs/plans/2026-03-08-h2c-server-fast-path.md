# H2c Server Active-Conn Fast Path Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a narrow internal inbound fast path for non-SSL server active-connection HTTP/2 so the hot `h2c` request path avoids unnecessary frame-object allocation and dispatch overhead.

**Architecture:** Introduce a lightweight raw frame-view batch reader for the active-conn server path, route hot `HEADERS` / `CONTINUATION` / `DATA` frames through direct stream-state mutation, and preserve the generic `Http2Frame::uptr` path as fallback for client mode, SSL, and uncommon frames. Keep public APIs unchanged and validate behavior with focused HTTP/2 active-conn tests before re-running the strict fairness `h2c` benchmark.

**Tech Stack:** C++23, `galay-http/kernel/http2/Http2Conn.h`, `galay-http/kernel/http2/Http2StreamManager.h`, existing HTTP/2 active-conn tests and `benchmark/B3-H2cServer.cc`.

---

### Task 1: Lock the server fast-path contract with a failing test

**Files:**
- Add: `test/T54-h2c_server_fast_path.cc`
- Test: `test/T43-h2_active_conn_preferred.cc`
- Test: `test/T52-h2_chunked_request_body.cc`

**Step 1: Write the failing test**

Require:

- server active-conn handling still observes a single completed request delivery for a `HEADERS + DATA + END_STREAM` request
- request body remains chunk-first and directly claimable
- the hot path does not depend on the stream frame queue being enabled

**Step 2: Run test to verify it fails**

Run:

```bash
cmake --build build --target T54-h2c_server_fast_path -j4
./build/test/T54-h2c_server_fast_path
```

Expected: FAIL because the new fast-path-specific contract is not implemented yet.

### Task 2: Add a raw inbound frame-view batch surface

**Files:**
- Modify: `galay-http/kernel/http2/Http2Conn.h`
- Test: `test/T54-h2c_server_fast_path.cc`

**Step 1: Extend the failing test or compile guard**

Require an internal batch surface that can expose lightweight frame views without constructing `Http2Frame::uptr` for every hot-path frame.

**Step 2: Run test/build to verify RED**

Run:

```bash
cmake --build build --target T54-h2c_server_fast_path -j4
```

Expected: compile failure or targeted assertion failure for the missing raw batch surface.

**Step 3: Write the minimal implementation**

Implement:

- lightweight raw frame-view type
- batch reader that validates frame boundaries and exposes header + payload views
- conservative fallback for cases that do not fit the first fast-path iteration

**Step 4: Run test to verify GREEN**

Run:

```bash
./build/test/T54-h2c_server_fast_path
```

Expected: PASS for the raw-view surface requirements.

### Task 3: Route hot server active-conn frames through the fast path

**Files:**
- Modify: `galay-http/kernel/http2/Http2StreamManager.h`
- Test: `test/T43-h2_active_conn_preferred.cc`
- Test: `test/T46-h2_active_conn_retire.cc`
- Test: `test/T53-h2_active_batch_flush.cc`
- Test: `test/T54-h2c_server_fast_path.cc`

**Step 1: Write/extend the failing test**

Require:

- `HEADERS`, `CONTINUATION`, and `DATA` on the server active-conn hot path mutate `Http2Stream` state directly
- merged event bits stay unchanged
- active-stream delivery count stays unchanged

**Step 2: Run test to verify RED**

Run:

```bash
cmake --build build --target \
  T43-h2_active_conn_preferred \
  T46-h2_active_conn_retire \
  T53-h2_active_batch_flush \
  T54-h2c_server_fast_path -j4
./build/test/T43-h2_active_conn_preferred
```

Expected: FAIL until the stream manager fast dispatch is active.

**Step 3: Write the minimal implementation**

Implement:

- reader-loop branch that uses the raw batch surface only for `server + active-conn`
- direct hot-frame handlers for `HEADERS`, `CONTINUATION`, and `DATA`
- fallback to the current generic frame path for anything outside the narrow hot subset

**Step 4: Run test to verify GREEN**

Run:

```bash
./build/test/T43-h2_active_conn_preferred
./build/test/T46-h2_active_conn_retire
./build/test/T53-h2_active_batch_flush
./build/test/T54-h2c_server_fast_path
```

Expected: PASS.

### Task 4: Verify the surrounding H2 regression set

**Files:**
- No additional source files

**Step 1: Build the targeted regression set**

Run:

```bash
cmake --build build --target \
  T33-h2_stream_frame_api \
  T42-h2_active_conn_api \
  T43-h2_active_conn_preferred \
  T44-h2c_client_shutdown \
  T46-h2_active_conn_retire \
  T49-h2_outbound_segments \
  T52-h2_chunked_request_body \
  T53-h2_active_batch_flush \
  T54-h2c_server_fast_path -j4
```

Expected: build succeeds.

**Step 2: Run the targeted regression tests**

Run:

```bash
./build/test/T33-h2_stream_frame_api
./build/test/T42-h2_active_conn_api
./build/test/T43-h2_active_conn_preferred
./build/test/T44-h2c_client_shutdown
./build/test/T46-h2_active_conn_retire
./build/test/T49-h2_outbound_segments
./build/test/T52-h2_chunked_request_body
./build/test/T53-h2_active_batch_flush
./build/test/T54-h2c_server_fast_path
```

Expected: PASS.

### Task 5: Re-run the strict fairness `h2c` benchmark

**Files:**
- Output only: `benchmark/results/<timestamp>-h2c-server-fast-path/`

**Step 1: Rebuild the benchmark target**

Run:

```bash
cmake --build build-ssl-nolog --target B3-H2cServer -j4
```

Expected: PASS.

**Step 2: Run the strict fairness `h2c` case**

Use:

- Galay `io=4`
- Galay `compute=0`
- Rust `TOKIO_WORKER_THREADS=4`
- `140` conns
- `8s`
- `128B`

Expected: a fresh result directory containing `galay_h2c.run.log`, `rust_h2c.run.log`, and `aggregate.csv`.

**Step 3: Compare against the approved strict baseline**

Compare with:

- `benchmark/results/20260308-110435-strict-runtime-fairness/aggregate.csv`

Report:

- new Galay `h2c` rps
- new Rust `h2c` rps from the same fairness setup
- remaining percentage gap
- whether the fast path is worth keeping or should be revised further
