# H2c Request-Complete Delivery Gate Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Reduce active-handler wake/resume churn on the non-SSL server HTTP/2 hot path by delivering server active-conn streams only once the request is complete, while preserving merged request event bits.

**Architecture:** Keep accumulating `HeadersReady` / `DataArrived` / `RequestComplete` bits on `Http2Stream`, but teach `Http2StreamManager` to hold back server active-conn deliveries until `RequestComplete` is present. Preserve public APIs and benchmark code; change only the internal delivery gate in the manager.

**Tech Stack:** C++23, `galay-http/kernel/http2/Http2StreamManager.h`, active-conn tests, `benchmark/B3-H2cServer.cc`.

---

### Task 1: Lock request-complete-gated delivery with a failing test

**Files:**
- Add: `test/T57-h2_active_complete_gate.cc`
- Test: `test/T43-h2_active_conn_preferred.cc`
- Test: `test/T46-h2_active_conn_retire.cc`

**Step 1: Write the failing test**

Require:
- in `server + active-conn` mode, a stream marked only with `HeadersReady` is not yet visible to `getActiveStreams()`
- once `RequestComplete` is merged in later, the same stream becomes visible exactly once
- delivered events still include the previously accumulated `HeadersReady` / `DataArrived` bits

**Step 2: Run test to verify RED**

Run:

```bash
cmake -S . -B build
cmake --build build --target T57-h2_active_complete_gate -j4
./build/test/T57-h2_active_complete_gate
```

Expected: FAIL because partial server-active deliveries are still allowed.

**Step 3: Implement the minimal gate**

Implement:
- server-active-only filter in the active flush path
- retain non-complete streams in the pending batch state so later events merge onto the same stream
- preserve close behavior and mailbox semantics

**Step 4: Run test to verify GREEN**

Run:

```bash
./build/test/T57-h2_active_complete_gate
```

Expected: PASS.

### Task 2: Verify the active `h2c` regression set

**Files:**
- No additional source files

**Step 1: Build the targeted regression set**

Run:

```bash
cmake --build build --target \
  T42-h2_active_conn_api \
  T43-h2_active_conn_preferred \
  T46-h2_active_conn_retire \
  T53-h2_active_batch_flush \
  T54-h2c_server_fast_path \
  T55-h2_active_batch_mailbox \
  T56-h2_stream_pool \
  T57-h2_active_complete_gate -j4
```

Expected: build succeeds.

**Step 2: Run the targeted regression tests**

Run:

```bash
./build/test/T42-h2_active_conn_api
./build/test/T43-h2_active_conn_preferred
./build/test/T46-h2_active_conn_retire
./build/test/T53-h2_active_batch_flush
./build/test/T54-h2c_server_fast_path
./build/test/T55-h2_active_batch_mailbox
./build/test/T56-h2_stream_pool
./build/test/T57-h2_active_complete_gate
```

Expected: PASS.

### Task 3: Re-run the strict fairness `h2c` benchmark

**Files:**
- Output only: `benchmark/results/<timestamp>-h2c-request-complete-gate/`

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

Expected: fresh result directory with logs and `aggregate.csv`.

**Step 3: Report measured delta**

Report:
- Galay `h2c` rps
- Rust `h2c` rps from same shape
- remaining gap
- whether one-delivery-per-complete-request is worth keeping
