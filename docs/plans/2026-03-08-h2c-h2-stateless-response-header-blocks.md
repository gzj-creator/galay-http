# H2/H2c Stateless Response Header Blocks Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Reduce `h2c` and `h2` response-side CPU overhead by adding a cacheable, state-independent HPACK encoding path for common response headers, then use it in the benchmark echo servers.

**Architecture:** Keep existing dynamic-table HPACK behavior as the default for generic HTTP/2 responses. Add a new stateless/no-index encoding path that only relies on the HPACK static table and never mutates dynamic table state. Expose a stream send path that accepts a pre-encoded header block directly, so benchmark handlers can prebuild the fixed echo response header block once and bypass per-request HPACK work.

**Tech Stack:** `galay-http/protoc/http2/Http2Hpack.*`, `galay-http/kernel/http2/Http2Stream.h`, benchmark servers `benchmark/B3-H2cServer.cc` and `benchmark/B12-H2Server.cc`, targeted tests in `test/`.

---

### Task 1: Lock stateless HPACK encoding with a failing test

**Files:**
- Add: `test/T51-hpack_stateless_encode.cc`

**Step 1: Write the failing test**

Require:
- `HpackEncoder` exposes a stateless/no-index encoding API
- encoded bytes decode back to the same headers
- calling the stateless API does not grow dynamic table state

**Step 2: Verify RED**

Build and run only `T51-hpack_stateless_encode`; expect failure for missing API / behavior.

### Task 2: Lock pre-encoded stream header sending with a failing test

**Files:**
- Modify or add: `test/T33-h2_stream_frame_api.cc`
- Modify or add: `test/T49-h2_outbound_segments.cc`

**Step 1: Write the failing test**

Require:
- `Http2Stream` exposes a direct send path for an already-encoded header block
- the outgoing packet is segmented and flattens to the expected HEADERS bytes

**Step 2: Verify RED**

Build and run the smallest target set until it fails for the expected missing API reason.

### Task 3: Implement stateless HPACK response encoding

**Files:**
- Modify: `galay-http/protoc/http2/Http2Hpack.h`
- Modify: `galay-http/protoc/http2/Http2Hpack.cc`

**Step 1: Add a no-index/state-independent encoder**

Implement an API that:
- uses indexed representation only for full static-table matches
- otherwise uses literal-without-indexing with static-name index when available
- never mutates dynamic table state

**Step 2: Keep existing behavior unchanged**

The current `encode()` path remains dynamic-table aware and backward compatible.

### Task 4: Implement pre-encoded HEADERS send path

**Files:**
- Modify: `galay-http/kernel/http2/Http2Stream.h`

**Step 1: Add direct header-block send helper**

Queue a segmented outgoing frame from caller-owned encoded header bytes without invoking `HpackEncoder` again.

**Step 2: Preserve stream semantics**

Keep `end_stream`, `end_headers`, waiter, and stream state transitions identical to the existing header send path.

### Task 5: Use cached header blocks in h2c/h2 benchmarks

**Files:**
- Modify: `benchmark/B3-H2cServer.cc`
- Modify: `benchmark/B12-H2Server.cc`

**Step 1: Precompute the common response header block**

Cache the fixed echo response headers for the benchmark payload size and send them through the new direct header-block path.

**Step 2: Keep correctness fallback**

If body size differs from the cached size, fall back to the existing dynamic header path.

### Task 6: Verify and re-baseline `h2c` / `h2`

**Files:**
- Output only: `benchmark/results/<timestamp>-h2-stateless-header-blocks/`

**Step 1: Run targeted tests**

At minimum:
- `T33-h2_stream_frame_api`
- `T42-h2_active_conn_api`
- `T49-h2_outbound_segments`
- `T51-hpack_stateless_encode`

**Step 2: Rebuild benchmark binaries**

Rebuild `B3-H2cServer` and `B12-H2Server`.

**Step 3: Re-run `h2c` / `h2` benchmark**

Compare against the strict-runtime fairness baseline and report the delta.

## Execution note

The user approved `h2c/h2` optimization first, so this plan is executed immediately in the current session.
