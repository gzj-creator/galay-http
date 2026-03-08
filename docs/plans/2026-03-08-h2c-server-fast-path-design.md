# H2c Server Active-Conn Fast Path Design

**Date:** 2026-03-08

**Status:** Approved for implementation

## Goal

Reduce the remaining non-SSL `h2c` service-side gap by removing avoidable inbound frame-object work on the hottest server path:

- server mode
- active-connection handler mode
- non-SSL socket path
- common request shape: `HEADERS (+ optional CONTINUATION) + DATA + END_STREAM`

The public API stays unchanged. SSL, client mode, and uncommon frame types remain on the current generic path.

## Current Problem

The current `h2c` reader flow still pays for the full generic HTTP/2 frame pipeline even on the benchmark hot path:

1. `Http2Conn::readFramesBatch()` parses bytes into `Http2Frame::uptr`
2. `Http2StreamManager::readerLoop()` iterates those heap-backed frame objects
3. `dispatchStreamFrame()` re-dispatches by frame type
4. `handleHeadersFrame()` / `handleDataFrame()` copy the relevant payload into stream state
5. active-conn mode suppresses the frame queue anyway, so the created frame objects are immediately discarded after state extraction

This means the active-conn server path still pays for:

- heap allocation of frame objects
- virtual-ish type branching through the generic frame layer
- string/object setup work that is not needed after request state has been materialized

The recent chunk-first request-body work improved body ownership, but the benchmark evidence shows the gap remains even for low-body or zero-body cases, so inbound frame orchestration is still a real contributor.

## Scope

### In scope

- `server + active-conn + non-SSL(h2c)` fast path
- internal read/dispatch optimization only
- preserving current `Http2Stream` request/event semantics
- preserving current active-batch delivery semantics

### Out of scope

- SSL / `h2`
- client mode
- changing public APIs
- changing benchmark fairness rules
- changing uncommon HTTP/2 frame behavior beyond routing them to the existing generic path

## Design Summary

Add an internal fast path that bypasses `Http2Frame::uptr` materialization for the hot subset of inbound server frames.

The fast path will:

- operate only when `m_active_conn_mode && !m_conn.isClient()`
- decode frame headers directly from the batch-read byte span
- handle hot `HEADERS`, `CONTINUATION`, and `DATA` frames in-place
- update `Http2Stream` request state directly
- merge active-stream notifications through the existing `Http2ActiveStreamBatch`
- fall back to the current generic frame-object path for anything complex or unsupported

This keeps correctness risk bounded while targeting the highest-volume path first.

## Why This Approach

### Option A: Active-conn server raw parse fast path

Pros:

- attacks the remaining `h2c` hot path directly
- removes frame-object churn rather than micro-optimizing around it
- preserves the external API and most internal control flow
- naturally composes with the existing chunk-first request body and outbound batch helpers

Cons:

- more internal parsing logic in the stream manager / conn layer
- requires careful fallback behavior for unusual frames

### Option B: More writer batching first

Pros:

- can help `HEADERS + DATA` response shape

Cons:

- does not address the inbound parse/object cost already shown by zero-body tests
- current `galay-kernel` timeout batching utility is awkward for move-only outbound packets

### Option C: Small-object pooling / retire / flow-control tuning first

Pros:

- lower implementation risk

Cons:

- likely incremental only
- does not match the observed magnitude of the remaining `h2c` gap

Recommended: **Option A**

## Detailed Architecture

### 1. Add a raw server-batch reader surface

The existing `readFramesBatch()` always returns parsed `Http2Frame::uptr` objects. The fast path needs a lower-level surface that still validates frame boundaries but does not instantiate frame objects for the common path.

The new internal surface should expose a batch of lightweight frame views containing:

- parsed `Http2FrameHeader`
- payload bytes as a `std::string_view` or equivalent borrowed view into the ring-buffer-visible storage when contiguous
- a signal indicating whether the payload had to be copied into a scratch buffer

The first implementation can stay conservative:

- contiguous frames take the fast path
- cross-iovec frames may either use a scratch buffer view or fall back to the generic parser

### 2. Teach the stream manager to consume hot frame views

In `server + active-conn` mode, the reader loop should first try the fast dispatch path:

- `HEADERS`: validate stream rules, append header-block bytes directly to the stream, apply priority bits if present, and complete request headers when `END_HEADERS` arrives
- `CONTINUATION`: append additional header-block bytes and complete the header decode on `END_HEADERS`
- `DATA`: update flow-control windows, move/copy payload directly into `Http2ChunkedBody`, and mark `RequestComplete` on `END_STREAM`

For these fast-path frames, no `Http2Frame::uptr` should be created, and no frame queue push should happen.

### 3. Preserve the generic path as fallback

Any frame that does not fit the intended hot path should fall back to the existing generic logic:

- connection-level frames
- client mode
- non-active-conn mode
- malformed or unsupported frame combinations
- frame types such as `RST_STREAM`, `WINDOW_UPDATE`, `PUSH_PROMISE`, and others

This preserves protocol coverage and keeps the first fast-path iteration small.

### 4. Keep existing event and delivery semantics

The optimization must not change the currently accepted active-conn behavior:

- same merged event bits
- same body chunk ownership semantics
- same `RequestComplete` visibility
- same one-delivery-per-reader-batch expectation where already locked by tests

The existing `Http2ActiveStreamBatch` remains the only active-stream delivery mechanism.

## Data Flow

### Before

`socket bytes`
-> `readFramesBatch()`
-> `Http2Frame::uptr`
-> `dispatchStreamFrame()`
-> `handleHeadersFrame()/handleDataFrame()`
-> mutate `Http2Stream`
-> `flushActiveStreams()`
-> active handler

### After on the hot path

`socket bytes`
-> raw frame-view batch
-> fast dispatch for `HEADERS/CONTINUATION/DATA`
-> mutate `Http2Stream`
-> `flushActiveStreams()`
-> active handler

### After on fallback

unchanged generic frame-object path

## Correctness Rules

The fast path must preserve:

- connection-level frame validation
- stream existence / creation rules
- continuation-state validation
- flow-control accounting
- header decode behavior
- end-stream state transitions
- stream retirement behavior

If any of those checks become awkward in the fast path, the code should fall back to the generic path instead of re-implementing more protocol surface than necessary.

## Risks and Mitigations

### Risk: duplicate protocol logic diverges

Mitigation:

- fast path handles only the hottest narrow subset
- complex or uncommon frames fall back immediately
- focused tests lock accepted behavior

### Risk: borrowed payload views outlive source storage

Mitigation:

- only borrow within the reader batch
- move/copy request payload into `Http2ChunkedBody` before the ring buffer advances
- use scratch storage or fallback when contiguous access is unavailable

### Risk: continuation / header decode edge cases

Mitigation:

- preserve existing stream-level header-block accumulation
- reuse existing completion logic (`completeReceivedHeaders`) after direct append

## Testing Strategy

### Focused regression

Add a new HTTP/2 active-conn test that proves:

- request completion still arrives once per reader batch
- request body chunk ownership is unchanged under the fast path
- no frame-queue dependency leaks into active-conn server handling

### Existing regression set

At minimum:

- `T42-h2_active_conn_api`
- `T43-h2_active_conn_preferred`
- `T44-h2c_client_shutdown`
- `T46-h2_active_conn_retire`
- `T49-h2_outbound_segments`
- `T52-h2_chunked_request_body`
- `T53-h2_active_batch_flush`

### Benchmark

Re-run the strict fairness `h2c` benchmark with:

- Galay `io=4`
- Galay `compute=0`
- Rust `TOKIO_WORKER_THREADS=4`
- `140` connections
- `8s`
- `128B`

Compare only against `benchmark/results/20260308-110435-strict-runtime-fairness/aggregate.csv`.

## Expected Outcome

The likely wins come from removing:

- frame heap allocations
- frame-object teardown
- generic dispatch overhead on the active-conn hot path

This should materially improve `h2c` beyond the current post-HPACK-batch result, while keeping the risk bounded and preserving all already-approved public behavior.
