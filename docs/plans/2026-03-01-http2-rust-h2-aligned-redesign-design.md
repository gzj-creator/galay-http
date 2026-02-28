# HTTP/2 Rust-h2 Aligned Redesign (Design)

## Background

Current HTTP/2 code has accumulated multiple concurrency and API styles. The project now requires a full rewrite that aligns behavior with mainstream Rust `h2` style: a connection-level event loop with deterministic frame dispatch.

This redesign is intentionally breaking: no compatibility adapter for old HTTP/2 APIs.

## Goal

Build a new HTTP/2 implementation that:

1. Uses a single connection-level loop as the protocol authority.
2. Preserves strict HTTP/2 framing and state-machine semantics.
3. Exposes frame-centric stream APIs (`replyHeader`, `replyData`, `getFrame`).
4. Removes request/response aggregate helpers from core flow (`readRequest`, `readResponse`).

## Non-Goals

1. Keep old HTTP/2 API compatibility.
2. Preserve old internal execution model (reader/writer/monitor split as architecture boundary).
3. Implement HTTP/3/QUIC scope.

## Scope

Full rewrite:

- `galay-http/protoc/http2/*`
- `galay-http/kernel/http2/*`

And synchronized API updates in:

- tests
- examples
- benchmarks
- docs
- module exports

## Architecture Overview

### 1) Connection Core

Per TCP/TLS connection, one protocol core coroutine owns protocol progression:

`read_frame -> apply_frame -> drive_stream_events -> schedule_outbound -> flush_socket -> check_timers`

The connection core is the only authority for:

- connection state transitions
- stream table lifecycle
- control-frame generation
- flow-control accounting
- socket writes

### 2) Frame Dispatch Model

Inbound frames are decoded and dispatched through a deterministic dispatcher:

- validate frame constraints by connection/stream state
- apply state transitions
- enqueue stream events
- enqueue required protocol responses (ACK, WINDOW_UPDATE, RST_STREAM, GOAWAY)

### 3) Stream State Objects

Each stream is a state container and event interface, not a socket owner.

Stream holds:

- stream FSM state
- send/recv window
- pending inbound frame events
- pending outbound app commands

### 4) Outbound Scheduler

Single outbound scheduler chooses what to send next, under:

- connection window
- stream window
- max frame size
- control-frame priority
- per-stream fairness/weight

### 5) Timer Integration

Timer checks are part of connection loop responsibilities:

- SETTINGS ACK timeout
- keepalive PING and ACK timeout
- graceful shutdown deadlines

## API Design (Breaking)

Frame-first API surface:

- `co_await stream->replyHeader(headers, end_stream)`
- `co_await stream->replyData(data, end_stream)`
- `co_await stream->getFrame()`
- `co_await stream->replyRst(error)` (or equivalent reset API)

Removed from primary usage:

- `readRequest`
- `readResponse`

Reason: HTTP/2 semantics are frame/event driven; aggregate request/response helpers mask important protocol transitions.

## Behavior Alignment Targets (Rust h2 Style)

1. Single connection protocol authority.
2. No direct socket write from stream handlers.
3. Ordered handling for HEADERS/CONTINUATION sequences.
4. Strict separation of connection-level vs stream-level errors.
5. Connection draining via GOAWAY semantics.
6. Flow-control correctness under multiplexed streams.

## Error Model

Unified explicit errors for awaitables:

- protocol violations
- flow-control violations
- stream closed/reset
- peer closed
- timeout

Policy:

- connection-level protocol faults => GOAWAY + close path
- stream-level faults => RST_STREAM (connection survives)

## Rewrite Phases

1. Rebuild protocol layer (`protoc/http2`): frame codec, hpack, error taxonomy, invariants.
2. Rebuild kernel layer (`kernel/http2`): connection core, stream state, dispatcher, outbound scheduler.
3. Rebuild public HTTP/2 client/server surfaces.
4. Update tests/examples/benchmarks/docs/modules to new APIs.
5. Remove legacy HTTP/2 code paths.

## Verification Strategy

1. Unit tests for frame parsing/serialization and HPACK behavior.
2. State-machine tests for stream transitions and connection transitions.
3. Flow-control tests (conn + stream windows, WINDOW_UPDATE behavior).
4. Integration tests for h2c and h2 TLS.
5. Regression tests for GOAWAY, RST_STREAM, SETTINGS/PING timeout behaviors.

## Acceptance Criteria

1. New APIs compile and run across h2c and h2 targets.
2. Legacy aggregate APIs are absent from new HTTP/2 surface.
3. All HTTP/2 tests/examples updated and passing.
4. Connection-loop + frame-dispatch model is the only HTTP/2 execution model in tree.
