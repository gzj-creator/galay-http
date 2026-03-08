# H2c Request-Complete Delivery Gate Design

**Problem:** The current server active-connection path may still wake and resume the active handler before a request is complete when `HEADERS` and `DATA` arrive in separate reader cycles. Even with merged event bits inside a single reader batch, cross-batch partial deliveries still create avoidable active-handler wakeups and coroutine resumes.

**Proposed Optimization:** In `server + active-conn` mode, keep accumulating `HeadersReady` / `DataArrived` bits on `Http2Stream`, but only release a stream to the active-handler handoff path once its pending events include `RequestComplete` (or other terminal conditions such as reset/close if needed later). This keeps event information intact while turning the hot request path into one delivery per completed request.

## Why This Fits The Current Contracts

- Existing hot-path benchmark `B3-H2cServer` only acts on `RequestComplete`.
- Current active-conn tests (`T43`, `T46`) already assert one delivery per request and only require that merged event bits are visible when that delivery happens.
- Public APIs remain unchanged: `Http2ConnContext::getActiveStreams(max_count)` still returns batches of `Http2Stream::ptr` with merged event bits.

## Scope

- Apply only to `server + active-conn` delivery.
- Leave client mode and legacy per-stream handler mode untouched.
- Keep internal mailbox/batch surfaces unchanged except for what streams are allowed through.

## Risk

This intentionally delays visibility of partial request-body progress to the active handler. That is a semantic tightening compared with the original queue model, but it matches the accepted benchmark path and the currently locked tests.
