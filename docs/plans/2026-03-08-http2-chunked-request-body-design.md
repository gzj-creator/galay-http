# HTTP/2 Chunked Request Body Design

**Date:** 2026-03-08

**Status:** Approved for implementation

**Goal**

Replace the current HTTP/2 request-body model that eagerly aggregates DATA frames into a single `std::string` with a chunk-first model that retains owned DATA chunks until application code explicitly asks to coalesce them.

This design is aimed first at the non-SSL `h2c` benchmark path, where the remaining gap to Rust is now more about read-side aggregation, active-stream dispatch, and echo-path copying than about outbound frame serialization.

## Context

Current server-side active-connection HTTP/2 flow still does avoidable work on the read path:

- each DATA frame appends into `request.body`, causing repeated string growth/copy work
- active-connection mode already exposes event batching, but the application still receives a pre-coalesced body string instead of the original owned chunks
- the echo benchmark then writes back that already-coalesced string, which means the hot path is effectively `frame chunk -> append into big string -> send big string`

That differs from Rust-style high-performance stacks, which generally retain owned body chunks and only coalesce when application logic actually needs contiguity.

## Approaches considered

### Approach A: keep `std::string body`, optimize reserve/append only

Improve `content-length` pre-reserve, event batching, and first-chunk move while preserving the current public request-body API.

**Pros**

- lowest migration cost
- least API churn

**Cons**

- still fundamentally builds one big string per request
- echo path still pays unnecessary coalescing cost
- likely insufficient to close the remaining `h2c` gap to Rust

### Approach B: chunk-first request body with explicit coalescing helper

Store request bodies as owned chunks and expose APIs to either claim chunks directly or explicitly coalesce them when needed.

**Pros**

- removes default read-side coalescing cost
- enables zero-extra-copy echo for batch/send-data-chunks paths
- still offers an escape hatch for handlers that need contiguous bodies
- matches the benchmark-first direction and Rust-like data flow

**Cons**

- breaks request-body API
- requires touching handlers/tests that assumed `request.body` is a string

### Approach C: generalized `Bytes`-like body type across the entire stack

Introduce a new shared/ref-counted byte abstraction and thread it through request, response, websocket, and HTTP/2 surfaces.

**Pros**

- most flexible long-term representation
- closest conceptual model to Rust `Bytes`

**Cons**

- much larger refactor than needed for the current bottleneck
- higher migration cost and more regression risk

## Chosen approach

Choose **Approach B**.

The codebase already has useful primitives for this path:

- `Http2DataFrame` already owns its payload string
- `Http2Stream` already has batch-oriented send helpers
- active-connection mode already exposes merged event delivery through `takeEvents()`

So the most direct path is to make request bodies chunk-first, preserve owned DATA payloads, and let handlers explicitly opt into coalescing only when necessary.

## Proposed architecture

### 1. Request body becomes an owned chunk container

Replace the string body field in `Http2Request` with a small owned-body type, for example:

- `std::vector<std::string> chunks`
- `size_t total_bytes`

Expose chunk-first helpers on the request/body surface:

- `bodySize()`
- `bodyChunkCount()`
- `bodyChunks()`
- `takeBodyChunks()`
- `coalesceBody()` / `takeCoalescedBody()` as explicit slow paths

The hot path no longer depends on eager coalescing.

### 2. Reader path moves DATA into request chunks

When server-side DATA frames arrive:

- move the owned payload string out of `Http2DataFrame`
- append it as a chunk into the request body container
- do not concatenate into a single string

This removes the repeated append/copy work that currently occurs in `appendRequestData(data->data())`.

### 3. Active-connection dispatch becomes batch-first by design

In active-connection mode, multiple events for the same stream inside one `readFramesBatch()` cycle should be merged and flushed once at the end of that reader batch.

That means a typical benchmark request that arrives as `HEADERS + DATA + END_STREAM` in the same reader batch can be delivered once with merged events:

- `HeadersReady`
- `DataArrived`
- `RequestComplete`

This keeps semantics correct while matching the intended batch-processing model.

### 4. Echo path sends chunks directly

Add move-based DATA chunk send helpers so handlers can echo request bodies without first coalescing them.

For the benchmark server, the path becomes:

1. request DATA chunks are retained as owned request chunks
2. handler claims them via `takeBodyChunks()`
3. response HEADERS use total byte size
4. response body is sent via chunk batch send

That is much closer to `chunk in -> chunk out` and avoids the previous `coalesce -> send` middle step.

## Data flow

### Old path

`DATA frame` -> append into `request.body` string -> `std::move(body)` in handler -> one DATA send

### New path

`DATA frame` -> move payload into request chunk list -> handler `takeBodyChunks()` -> `sendDataChunks(...)`

The new path avoids default request-body coalescing and lets the benchmark echo handler preserve the natural chunk boundary.

## API direction

This is an intentional major-version API break. Backward compatibility with the old `request.body` string model is not required.

That means we can:

- remove the old string-centric request-body hot path
- update tests/examples/benchmarks to the new chunk-first body model
- keep explicit coalescing helpers only where required for convenience

## Error handling and invariants

- empty request bodies remain valid and should report `bodySize() == 0`
- `takeBodyChunks()` must leave the request body empty and reset byte counters
- merged active-connection events must preserve correctness even when multiple frame types arrive in one reader batch
- non-active-connection stream-frame APIs remain semantically correct even if they still queue individual frames

## Testing strategy

1. Add/extend API contract tests for chunk-first request bodies
2. Add a focused active-connection batching test that proves one dispatch can carry merged `HEADERS + DATA + END_STREAM`
3. Add a focused body-ownership test for `takeBodyChunks()` and explicit coalescing helpers
4. Update `B3-H2cServer` to consume/request/send chunks directly
5. Re-run the strict-fairness `h2c` benchmark only after targeted tests pass

## Expected outcome

This should remove one of the biggest remaining user-space inefficiencies on non-SSL `h2c`:

- no default DATA-to-big-string aggregation on read
- fewer active-stream dispatches per request
- direct chunk echo instead of body re-materialization

It is the most plausible non-SSL path left for pulling `h2c` materially closer to Rust.
