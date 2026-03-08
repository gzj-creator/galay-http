# HTTP/2 Outbound Segments Design

**Date:** 2026-03-08

**Status:** Approved for implementation

**Goal**

Replace the current HTTP/2 outbound hot path that centers around fully serialized frame strings with a segmented outbound representation that keeps frame headers and payloads separate until the final socket write stage.

This design specifically targets the remaining non-SSL `h2c` gap first, but the data model is intentionally shared with `h2` so the same send-path improvements can later carry into TLS once SSL-specific write behavior is tuned.

## Context

Current `h2c` throughput is meaningfully closer to Go and Rust than before, but it still trails the Rust baseline. Profiling and code inspection show that the remaining gap is less about socket batching mechanics and more about the outbound frame representation:

- `HEADERS` currently pay for HPACK encoding and then another full-frame serialization into a new `std::string`
- `DATA` currently build a new contiguous frame string even when the payload already exists in stable memory
- the writer loop already supports efficient `writev`, but the producer side still feeds it a serialization-heavy model

The result is that the implementation still does extra allocation/copy work that Rust’s `h2` stack largely avoids by treating frame heads and body chunks separately.

## Reference: what Rust is effectively doing

The local Rust benchmark server uses Hyper/H2 with `Bytes`-backed bodies. Application code builds responses from `Bytes`, and profiling shows distinct hotspots in:

- HPACK header encoding
- HTTP/2 header frame encoding
- HTTP/2 data chunk encoding

That is important because it means Rust is not paying for a second “flatten the whole frame into one owned buffer” step on the hot path. It still encodes headers and frame metadata, but it keeps the send model closer to `{frame head, payload chunk}` than to `serialized_frame_string`.

## Approaches considered

### Approach A: incremental header/data helpers on top of `serialized`

Keep `Http2OutgoingFrame` string-centric and only add helper APIs that reduce a few local allocations.

**Pros**

- smallest patch
- easy to verify

**Cons**

- leaves the wrong data model in place
- gains are likely capped
- harder to reuse for `h2` and later SSL work

### Approach B: HPACK/result caching only

Keep frame serialization model but cache common response headers or benchmark-specific outputs.

**Pros**

- low risk
- fast to prototype

**Cons**

- benchmark-specific and brittle
- does not address DATA-path copying
- weaker carry-over to `h2`, `https`, or `wss`

### Approach C: outbound representation rewrite

Rewrite the send path so the hot path no longer requires building a single contiguous frame string before writing.

**Pros**

- addresses the real structural cost
- naturally matches `writev`
- reusable for both `h2c` and `h2`
- gives a clean foundation for later SSL-specific flatten-on-demand logic

**Cons**

- larger change set
- needs careful lifecycle handling
- must preserve exact frame ordering and waiter semantics

## Chosen approach

Choose **Approach C**, but in a constrained form:

- rewrite only the outbound representation and send-path plumbing
- do not change protocol state machines, flow control logic, or frame ordering semantics
- keep a compatibility fallback for existing pre-serialized/control-frame paths
- use `h2c` as the first acceptance gate before broadening optimization to TLS-backed `h2`

This is effectively a **C-lite representation rewrite**, not a full HTTP/2 architecture rewrite.

## Proposed architecture

### 1. Segmented outbound frame model

`Http2OutgoingFrame` becomes a send packet abstraction that can represent:

- **empty/shutdown sentinel**
- **fully serialized bytes** for compatibility and small control-frame fallback
- **segmented frame**
  - fixed `9`-byte HTTP/2 frame header
  - payload buffer retained separately

The segmented representation is the new hot path for `HEADERS` and `DATA`.

### 2. Frame builder support

`Http2FrameBuilder` keeps the current `dataBytes()/headersBytes()/rstStreamBytes()` helpers for compatibility and tests, but gains low-level helpers that produce only the frame header bytes needed by the segmented path.

This preserves byte-equivalence tests while allowing the producer side to stop flattening the full frame.

### 3. Stream send API evolution

`Http2Stream` send internals change as follows:

- `sendHeadersInternal(...)`
  - HPACK still produces one owned header block string
  - the frame is queued as `{9-byte header, owned HPACK block}`
  - the second “build a contiguous full-frame string” step disappears

- `sendDataInternal(...)`
  - current `const std::string&` API stays for compatibility
  - add move-based fast path so stable body buffers can be transferred into the outbound packet without another payload copy
  - the segmented representation means the frame no longer needs a second copy into `header+payload` storage

### 4. Writer loop consumption

`writerLoop()` consumes both legacy and segmented outbound packets:

- serialized packets contribute one `iovec`
- segmented packets contribute up to two `iovec`s:
  - header
  - payload

The existing partial-write cursor logic remains in place. The rewrite changes what the writer loop writes, not how partial `writev` progress is tracked.

### 5. TLS compatibility path

For `TcpSocket`, the writer loop directly writes segmented packets via `writev`.

For `SslSocket`, the initial version keeps a flatten-on-demand fallback:

- segmented packets are lazily linearized only at the SSL send boundary
- correctness stays unchanged
- the structural rewrite is still reused by `h2`
- SSL-specific optimization can happen later without redoing the data model

## Data ownership and lifetime

The first implementation targets safe ownership first:

- segmented `HEADERS` payloads are owned by the outbound packet
- segmented `DATA` supports owned payload transfer via move path
- compatibility overloads remain available when callers only have borrowed string references

This means the first cut eliminates the extra full-frame flattening cost even before every call site adopts move-based payload transfer.

The design intentionally leaves room for a later borrowed-payload mode with explicit ownership tokens if profiling proves it worthwhile.

## Behavioral invariants

The rewrite must not change:

- frame ordering
- stream/window accounting
- waiter notification semantics
- GOAWAY / shutdown behavior
- active-connection event delivery
- HTTP/2 wire bytes

The only intended changes are allocation/copy shape and writer-loop input form.

## Testing strategy

### Contract / byte-equivalence

- extend `test/T37-h2_frame_builder_bytes.cc`
  - segmented header helpers must produce bytes equivalent to the old full-frame builders once flattened

- extend `test/T33-h2_stream_frame_api.cc`
  - lock the new move-based fast-path surface

### New focused regression

- add `test/T49-h2_outbound_segments.cc`
  - segmented packet flattening
  - serialized-vs-segmented equivalence
  - partial-write-facing iovec shape
  - waiter retention semantics for segmented packets

### Existing protocol guards

Continue using:

- `test/T42-h2_active_conn_api.cc`
- `test/T43-h2_active_conn_preferred.cc`
- `test/T44-h2c_client_shutdown.cc`
- `test/T46-h2_active_conn_retire.cc`
- `test/T48-non_ssl_kernel_surface.cc`

## Rollout plan

1. Add segmented outbound representation with byte-equivalence tests
2. Make writer loop consume both serialized and segmented forms
3. Switch `HEADERS` send path to segmented form
4. Switch `DATA` send path and add move overloads
5. Re-baseline `h2c`
6. If `h2c` holds or improves, apply the same representation to `h2` with TLS fallback still enabled

## Acceptance criteria

The work is accepted when:

- all targeted H2 regression tests pass
- `h2c` benchmark remains correct and improves over the current baseline window
- the send path is representation-driven by segments rather than fully serialized frame strings for hot `HEADERS`/`DATA` traffic
- the same representation is usable by both `h2c` and `h2`, even if SSL still uses flatten-on-demand internally

## Notes

- No commit is created as part of this design handoff because commits were not requested in this session.
