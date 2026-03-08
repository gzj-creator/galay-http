# H2c Stream Pool Design

**Problem:** `h2c` server-active mode still creates and destroys a `Http2Stream` object for each request stream. The previous direct-batch handoff change showed that delivery handoff is not the dominant cost, so the next likely hot cost is stream lifecycle: `shared_ptr` target allocation, per-stream state construction, frame queue/waiter initialization, and request/response object churn.

**Scope:** Only optimize the server-side active-connection path first. Keep public APIs unchanged. Preserve behavior for client mode and legacy per-stream handler mode.

## Proposed Design

Introduce an internal `Http2StreamPool` that returns `Http2Stream::ptr` backed by pooled raw `Http2Stream` objects. The pool is owned by `Http2StreamManagerImpl` and only used for `server + active-conn` stream creation. When a stream is retired from the connection map and its final external `shared_ptr` reference is released, the custom deleter returns the object to the pool instead of deleting it.

On reuse, the stream object performs an explicit `resetForReuse(stream_id)` that:
- resets stream ID, state machine flags, flow-control windows, priority fields
- clears request/response/header/decode buffers while preserving capacity where possible
- reconstructs frame queue and async waiters so stale sentinels and notified states do not leak across requests
- detaches previous IO bindings and retire callback

## Why This Shape

- It attacks object lifecycle cost without changing user-facing active-handler APIs.
- It is safe with external `shared_ptr` ownership because pooling happens only when the final reference is dropped.
- It avoids changing `Http2ConnContext` or benchmark code again.
- It is narrow enough to validate with dedicated reuse tests before re-benchmarking.

## Risks

- Reusing a stream without fully resetting internal waiters/channel state can create extremely subtle cross-request corruption.
- Returning a stream to pool before all references are dropped would be a correctness bug; custom-deleter based return avoids that.
- If this still does not move `h2c`, the remaining gap is likely above `Http2Stream` itself, e.g. handler/resume model or deeper runtime costs.
