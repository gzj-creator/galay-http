# HTTP/2 Shared Outbound Buffers Design

**Date:** 2026-03-08

**Status:** Approved for implementation

**Goal**

Extend the HTTP/2 outbound packet model so HEADERS and DATA can retain shared payload storage instead of always materializing a per-send owned `std::string`, then use that capability where cached response header blocks already exist.

This is a follow-up to the segmented outbound packet work. The segmented model already removed whole-frame flattening on the non-SSL hot path; this design removes the remaining per-send payload ownership copy for reusable buffers.

## Context

Current outbound HTTP/2 behavior has three materially different cases:

- `sendData(std::string&&)` is already relatively efficient because the payload buffer is moved into the queued packet
- `sendHeaders(headers...)` still pays HPACK encode cost every time, which is unavoidable unless the header block is reused
- `sendEncodedHeaders(const std::string&)` re-copies the already cached header block into `owned_payload` on every send

That last point is exactly where the current `h2` benchmark still wastes work. The benchmark already caches a fixed response header block for the 128-byte echo case, but the send path copies that block once into the outgoing packet and then, under SSL, copies it again during flatten-on-send.

For `h2c`, a global stateless header cache is not a universal win because connection-local dynamic-table hot state can beat stateless encoding. But the transport layer should still support shared buffers generically, because it is the right representation for cached headers and later constant-body responses.

## Approaches considered

### Approach A: benchmark-only manual special case

Add one-off benchmark logic that bypasses packet ownership and sends directly from a global string.

**Pros**

- smallest immediate patch
- could target one benchmark quickly

**Cons**

- leaks benchmark-specific behavior into protocol code
- not reusable for general HTTP/2 responses
- does not improve the transport abstraction

### Approach B: generic shared payload support in `Http2OutgoingFrame`

Teach the outbound packet to hold either owned or shared payload bytes while preserving the existing serialized fallback.

**Pros**

- minimal protocol-surface change
- naturally fits the segmented packet model
- reusable by cached HEADERS and future fixed DATA responses
- keeps `writerLoop()` simple because it already consumes iovecs

**Cons**

- adds one more payload ownership mode to maintain
- needs exact tests so flatten/iovec behavior stays byte-for-byte correct

### Approach C: generalized reference-counted byte-span abstraction everywhere

Introduce a brand-new `Bytes`-like abstraction and thread it through HPACK, frame building, stream APIs, and writer logic.

**Pros**

- closest conceptual match to Rust `Bytes`
- most future flexibility

**Cons**

- much larger refactor
- touches too many unrelated paths at once
- higher regression risk for modest near-term gain

## Chosen approach

Choose **Approach B**.

The current codebase already has the right outer model: `Http2OutgoingFrame` can represent a segmented packet, and `writerLoop()` already sends iovec slices without flattening on non-SSL sockets. The missing piece is simply payload ownership flexibility.

So this design keeps the existing packet shape and adds one more payload mode:

- owned payload (`std::string`)
- shared payload (`std::shared_ptr<const std::string>`)
- serialized compatibility fallback

No protocol state machine changes, no writer scheduling changes, and no new generic byte container are required in this iteration.

## Proposed architecture

### 1. Shared payload-capable outbound packet

`Http2OutgoingFrame` gains shared-payload storage plus helpers that abstract payload access:

- `payloadData()`
- `payloadSize()`
- `segmentedShared(...)`

`flatten()` and `exportIovecs()` become ownership-agnostic: they expose either owned or shared payload bytes with identical serialized output.

### 2. Stream send overloads for shared buffers

`Http2Stream` gains overloads for:

- `sendEncodedHeaders(std::shared_ptr<const std::string>, ...)`
- `replyEncodedHeaders(std::shared_ptr<const std::string>, ...)`
- `sendData(std::shared_ptr<const std::string>, ...)`
- `replyData(std::shared_ptr<const std::string>, ...)`

These are transport-level helpers only. Existing `std::string` and header-vector APIs remain the default and backward compatible.

### 3. Writer loop stays representation-driven

`writerLoop()` should not need protocol changes. It already asks each packet for iovecs on the non-SSL path and calls `flatten()` only for the non-`writev` path.

The only requirement is that shared-payload segmented packets produce the same iovec layout and flatten bytes as owned-payload segmented packets.

### 4. Benchmark use is narrow and intentional

This iteration uses the new shared-buffer path only where it has clear value:

- `B12-H2Server` reuses one cached encoded response header block for the 128-byte benchmark payload through a shared pointer

`B3-H2cServer` intentionally stays on the current dynamic-header path because prior measurements showed the global stateless header cache loses to connection-local dynamic-table hot state.

## Data flow

For cached HEADERS in `B12`:

1. Build one shared encoded header block at process init
2. Each matching response queues `Http2OutgoingFrame::segmentedShared(header_bytes, shared_block)`
3. Non-SSL writer path exposes `{frame header, shared payload}` directly via iovecs
4. SSL fallback path flattens once at send time, but no longer pays the extra packet-ownership copy

For shared DATA callers in the future:

1. Application passes `shared_ptr<const std::string>` into `sendData(...)`
2. Flow-control accounting uses `data->size()`
3. Queued packet retains the shared storage until send completes

## Error handling and invariants

- Null shared pointers are treated as empty payloads and should not crash the send path
- Stream state transitions, waiter behavior, and queue ordering remain identical to existing send helpers
- `flatten()` output must stay byte-identical to current full-frame builders for both owned and shared segmented packets

## Testing strategy

1. Extend API-surface tests to require shared-buffer send overloads
2. Extend outbound-segment tests to prove shared packets flatten and export iovecs correctly
3. Add a focused regression test for shared DATA/header ownership if needed
4. Re-run the existing targeted HTTP/2 regression set after implementation
5. Rebuild and re-run `B12-H2Server` against the strict fairness client configuration

## Expected outcome

This change is not expected to close the entire `h2` gap by itself. It should, however:

- remove one avoidable copy from cached HEADERS sends
- establish the correct reusable transport primitive for later fixed-body and SSL-path work
- do so with low protocol risk and minimal code churn
