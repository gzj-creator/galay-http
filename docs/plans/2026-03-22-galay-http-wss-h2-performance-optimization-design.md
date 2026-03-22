# galay-http WSS/H2 Performance Optimization Design

**Date:** 2026-03-22

## Goal

Improve same-machine benchmark performance for `wss` and `h2` in `galay-http` while preserving correctness, readability, and the current `Task<void>`-only async surface.

The optimization target is not "beat Go/Rust at any cost". The target is:

- remove obvious benchmark-path architectural disadvantages
- reduce avoidable TLS-path write amplification and idle polling
- keep the framework surface stable for downstream repositories
- re-verify all `test` / `example` / `benchmark` targets after the changes

## Fresh Benchmark Evidence

The current same-environment comparison result is recorded in:

- `benchmark/results/20260322-174642-galay-go-rust-http-proto-compare/summary.md`
- `benchmark/results/20260322-174642-galay-go-rust-http-proto-compare/metrics.csv`

Relevant protocol results:

- `ws`: Galay `192434.75 rps`, Go `189221.12 rps`, Rust `191555.38 rps`
- `wss`: Galay `138206.50 rps`, Go `175585.62 rps`, Rust `173214.38 rps`
- `h2c`: Galay `120254.00 rps`, Go `74534.50 rps`, Rust `114860.88 rps`
- `h2`: Galay `2110.38 rps`, Go `73006.50 rps`, Rust `91475.75 rps`

This means:

- the cleartext WebSocket and HTTP/2 paths are already competitive
- the severe regression is concentrated in the TLS variants, especially `h2`
- `h2c` proving strong performance is evidence that the HTTP/2 core is not the primary bottleneck by itself

## Current Architecture Comparison

### Go

The compare server uses:

- `gorilla/websocket` for `ws` / `wss`
- `net/http` plus `http2.ConfigureServer` for `https` / `h2`
- `h2c.NewHandler(...)` for cleartext HTTP/2

Go keeps protocol handling inside mature runtime-integrated libraries. The benchmark path is narrow:

- upgrade once
- keep a single WebSocket connection state machine
- echo request bodies directly
- let the runtime and library own TLS / HTTP/2 scheduling

### Rust

The compare server uses:

- `axum` WebSocket upgrade for `ws` / `wss`
- `axum_server` + `rustls` for TLS
- `hyper::server::conn::http2::Builder` for `h2c`

Rust likewise delegates most protocol machinery to libraries that already batch and schedule around Tokio's runtime model.

### Galay

For the non-TLS fast path, `galay-http` already has a lower-overhead mode:

- `B3-H2cServer` uses `activeConnHandler`
- it receives ready streams in batches via `ctx.getActiveStreams(64)`
- it replies with `sendEncodedHeadersAndData(...)` or `sendEncodedHeadersAndDataChunks(...)`

But the TLS benchmark path still has two major disadvantages:

1. `B7-WssServer` does not use the framework's own `WsConn` / `WsReader` / `WsWriter` stack after upgrade. It falls back to manual `SslSocket` reads, a manually managed accumulation buffer, manual frame parsing, and per-frame send loops.

2. `B12-H2Server` still uses the older per-stream handler model and sends headers and data as separate operations, while the stronger `h2c` benchmark already uses the active-connection batch model and combined outbound APIs.

The shared TLS HTTP/2 core also has a structural cost:

- `Http2StreamManagerImpl<...>::sslServiceLoop(...)` is a single owner coroutine
- it serially drains outbound, parses inbound, dispatches frames, and polls for more TLS reads
- idle receive uses a fixed timeout-based loop (`5ms`) instead of a more demand-driven wake pattern
- outbound TLS writes flatten and send many small buffers one by one

## Root Cause Summary

The most likely causes of the current gap are:

### 1. Benchmark-path mismatch between TLS and non-TLS HTTP/2

`h2c` uses the optimized active-connection batch model while `h2` still uses a heavier stream-per-handler path. This makes the benchmark unfair inside the same framework before Go/Rust are even considered.

### 2. TLS write amplification

For `h2`, response headers and bodies are often emitted as separate outbound items. In the TLS owner loop those items are flattened and written in smaller units than necessary. That increases:

- TLS record churn
- coroutine wakeups
- send calls
- queue pressure

### 3. TLS owner-loop scheduling overhead

The current TLS owner loop handles both directions itself and falls back to timeout polling when no immediate work is visible. That is much more expensive than the cleartext path and significantly farther from the runtime-integrated behavior of Go/Rust.

### 4. WSS benchmark bypasses shared optimized WebSocket primitives

The non-TLS `ws` benchmark uses `WsConn` / `WsReader` / `WsWriter` and performs well. `wss` bypasses those shared abstractions and pays for duplicate buffer handling and manual parse/send control flow.

## Design Decisions

### 1. Make the WSS benchmark use the same WebSocket stack as WS

`B7-WssServer` will stop manually reading and parsing WebSocket frames from `SslSocket`.

Instead it will:

- upgrade HTTP to WebSocket once
- convert the connection to `WssConn`
- use `WsReader` / `WsWriter` for welcome, echo, ping/pong, and close

This keeps the benchmark representative of the real framework path and removes duplicated TLS-WebSocket glue logic.

### 2. Align the H2 benchmark with the proven H2C fast path

`B12-H2Server` will be changed to use `activeConnHandler`, matching `B3-H2cServer`.

Inside that handler it will:

- fetch ready streams in batches
- react only to completed requests
- reply with pre-encoded shared headers where possible
- use `sendEncodedHeadersAndData(...)` or `sendEncodedHeadersAndDataChunks(...)`

This removes the avoidable per-stream coroutine overhead from the benchmark path before deeper TLS work begins.

### 3. Reduce outbound TLS send fragmentation in `sslServiceLoop`

`Http2StreamManagerImpl<...>::sslServiceLoop(...)` will keep the single-owner safety model for `SslSocket`, but its outbound path will be made more batch-friendly:

- collect more ready `Http2OutgoingFrame` items per loop turn
- coalesce them into fewer contiguous TLS writes
- avoid unnecessary per-item flatten/send churn when multiple frames are already available

This is the lowest-risk shared optimization with the highest expected immediate benefit for `h2`.

### 4. Refine the TLS owner loop before considering deeper kernel changes

The first shared-loop rewrite will stay inside `Http2StreamManager.h`.

The loop should prefer this rhythm:

- drain outbound aggressively
- parse all currently buffered inbound frames
- only then block or idle

The current fixed timeout polling should be reduced or made more adaptive where possible. However, this round will not immediately change public kernel or SSL awaitable interfaces.

### 5. Defer deeper `galay-kernel` / `galay-ssl` API surgery unless the first round is insufficient

If the first round substantially improves `wss` and `h2`, the work stops there.

Only if `h2` remains badly behind after:

- benchmark path alignment
- combined response sends
- TLS outbound batching
- owner-loop scheduling cleanup

will the next round consider deeper changes such as explicit outbound-ready wake channels or SSL-specific scheduling primitives.

## Non-Goals

This round will not:

- reintroduce `Coroutine`
- add new public awaitable compatibility layers
- rewrite Go or Rust comparison servers
- optimize client-side benchmark code first
- change unrelated `http`, `https`, `ws`, or `h2c` fast paths unless required by shared code cleanup

## Verification Plan

The optimization is only considered complete after fresh evidence:

1. Build:
   - `cmake -S . -B build-ssl -DCMAKE_BUILD_TYPE=Release -DGALAY_HTTP_ENABLE_SSL=ON`
   - `cmake --build build-ssl --parallel 8`
   - `cmake -S . -B build-ssl-nolog -DCMAKE_BUILD_TYPE=Release -DGALAY_HTTP_ENABLE_SSL=ON -DGALAY_HTTP_DISABLE_FRAMEWORK_LOG=ON`
   - `cmake --build build-ssl-nolog --parallel 8`

2. Correctness:
   - rerun the focused `wss` / `h2` server-client tests
   - rerun all current-source `test` / `example` / `benchmark` verification

3. Performance:
   - rerun `benchmark/compare/protocols/run_compare.sh`
   - compare fresh `wss` / `h2` data against the current baseline

## Expected Outcome

The intended first-round outcome is:

- `wss` moves materially closer to Go/Rust by removing the manual benchmark-only stack
- `h2` stops behaving like an outlier and begins to track the strong `h2c` architecture more closely
- the code remains readable because the fast path is built by reusing existing abstractions rather than adding more ad hoc special cases
