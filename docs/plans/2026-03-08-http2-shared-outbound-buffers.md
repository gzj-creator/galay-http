# HTTP/2 Shared Outbound Buffers Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add shared payload support to HTTP/2 outbound packets so cached HEADERS and future fixed DATA responses can be queued without an extra per-send payload copy.

**Architecture:** Keep the current segmented packet model and serialized fallback. Extend `Http2OutgoingFrame` with a shared-payload mode, add matching `Http2Stream` overloads, then wire the cached `h2` benchmark response header block through the new API.

**Tech Stack:** `galay-http/kernel/http2/Http2Stream.h`, `galay-http/kernel/http2/Http2StreamManager.h`, `benchmark/B12-H2Server.cc`, focused tests in `test/`.

---

### Task 1: Lock the shared outbound API with failing tests

**Files:**
- Modify: `test/T33-h2_stream_frame_api.cc`
- Modify: `test/T49-h2_outbound_segments.cc`
- Add if needed: `test/T52-h2_shared_outbound_buffers.cc`

**Step 1: Write the failing test**

Require:
- `Http2Stream` exposes shared-buffer overloads for encoded HEADERS and DATA
- `Http2OutgoingFrame` exposes a shared segmented constructor/helper
- shared segmented packets flatten to the same bytes as owned segmented packets
- shared segmented packets export iovecs that point at the shared buffer payload

**Step 2: Verify RED**

Run:

```bash
cmake --build build --target T33-h2_stream_frame_api T49-h2_outbound_segments -j4
./build/test/T49-h2_outbound_segments
```

Expected: FAIL for missing shared-buffer API/behavior.

### Task 2: Implement shared-payload packet support

**Files:**
- Modify: `galay-http/kernel/http2/Http2Stream.h`
- Modify if required: `galay-http/kernel/http2/Http2StreamManager.h`
- Test: `test/T49-h2_outbound_segments.cc`

**Step 1: Add minimal shared payload representation**

Implement:
- shared payload storage on `Http2OutgoingFrame`
- `segmentedShared(...)`
- ownership-agnostic payload access inside `flatten()` and `exportIovecs()`

**Step 2: Keep writer semantics unchanged**

If `writerLoop()` needs changes, keep them minimal and representation-only.

**Step 3: Verify GREEN**

Run:

```bash
./build/test/T49-h2_outbound_segments
```

Expected: PASS.

### Task 3: Implement shared HEADERS/DATA send overloads

**Files:**
- Modify: `galay-http/kernel/http2/Http2Stream.h`
- Test: `test/T33-h2_stream_frame_api.cc`
- Test: `test/T49-h2_outbound_segments.cc`

**Step 1: Write/finish the failing test**

Require:
- `sendEncodedHeaders(shared_ptr<const std::string>, ...)`
- `replyEncodedHeaders(shared_ptr<const std::string>, ...)`
- `sendData(shared_ptr<const std::string>, ...)`
- `replyData(shared_ptr<const std::string>, ...)`

**Step 2: Implement the minimal overloads**

Queue segmented shared packets while preserving:
- `end_stream`
- waiter behavior
- flow-control accounting
- existing overload behavior

**Step 3: Verify GREEN**

Run:

```bash
./build/test/T33-h2_stream_frame_api
./build/test/T49-h2_outbound_segments
```

Expected: PASS.

### Task 4: Use shared cached HEADERS in `B12`

**Files:**
- Modify: `benchmark/B12-H2Server.cc`

**Step 1: Switch cached header block ownership**

Store the cached encoded response header block as `std::shared_ptr<const std::string>` and send it through the new shared overload.

**Step 2: Keep fallback correctness**

If body size differs from the benchmark size, keep the current dynamic header path unchanged.

**Step 3: Verify build**

Run:

```bash
cmake --build build-ssl-nolog --target B12-H2Server -j4
```

Expected: PASS.

### Task 5: Run the targeted regression and benchmark check

**Files:**
- Output only: `benchmark/results/<timestamp>-h2-shared-outbound-buffers/`

**Step 1: Run targeted tests**

At minimum:
- `T33-h2_stream_frame_api`
- `T42-h2_active_conn_api`
- `T49-h2_outbound_segments`

**Step 2: Re-run the `h2` fairness case**

Use the same strict comparison setup:
- `4` IO threads
- `0` compute threads for Galay
- `140` conns
- `8s`
- `128B` payload

**Step 3: Compare with the strict baseline**

Compare against:
- `benchmark/results/20260308-110435-strict-runtime-fairness/aggregate.csv`

Report the delta and whether the shared-buffer path is worth keeping.
