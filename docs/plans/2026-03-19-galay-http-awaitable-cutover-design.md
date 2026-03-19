# galay-http Awaitable Cutover Design

**Date:** 2026-03-19

## Goal

Complete the `galay-http` Awaitable convergence now that `galay-kernel` `AwaitableBuilder` supports `readv` and `writev`, so protocol-facing socket IO flows no longer depend on hand-written `CustomAwaitable` state machines in this repository.

The cutover is intentionally aggressive:

- Public protocol APIs may change their concrete awaitable return types.
- `galay-http` custom awaitable class names should disappear from the public surface where the flow is fundamentally a socket IO state machine.
- No compatibility aliases will be kept just to preserve old type names.

## Background

`galay-kernel` already exposes a shared state-machine core:

- `StateMachineAwaitable<MachineT>`
- `AwaitableBuilder<ResultT, InlineN, FlowT>`
- `MachineAction::waitRead(...)`
- `MachineAction::waitWrite(...)`
- `MachineAction::waitReadv(...)`
- `MachineAction::waitWritev(...)`
- `MachineAction::waitConnect(...)`

`galay-ssl` already exposes the matching SSL-side family:

- `SslStateMachineAwaitable<MachineT>`
- `SslAwaitableBuilder<ResultT, InlineN, FlowT>`

That means `galay-http` no longer needs its own protocol-local `CustomAwaitable + addTask() + handleComplete()` orchestration for socket-driven protocol flows.

## Design Decisions

### 1. Public APIs switch directly to shared state-machine awaitables

Protocol APIs in `galay-http` should return the shared awaitable families directly:

- TCP flows return `StateMachineAwaitable<...>`
- SSL flows return `SslStateMachineAwaitable<...>`

The repository will not preserve old public awaitable names through wrappers or `using` aliases. Callers should consume these APIs through `auto` or direct `co_await`.

### 2. Scope is limited to socket-driven protocol flows

This cutover applies to awaitables whose behavior is fundamentally built from:

- `recv`
- `send`
- `readv`
- `writev`
- `connect`
- SSL `handshake`
- SSL `recv`
- SSL `send`
- SSL `shutdown`
- local parse / finish transitions between those steps

This includes the main protocol families:

- HTTP readers, writers, and request/response session flows
- WebSocket upgrade, frame/message read, and frame write flows
- HTTP/2 frame read/write flows and client connect/request/upgrade flows

### 3. Mailbox/waiter awaitables are explicit exceptions

Not every awaitable in the repository is a socket IO state machine. Internal coordination primitives such as:

- `Http2ActiveStreamMailbox::RecvBatchAwaitable`
- `Http2StreamManager::ShutdownAwaitable`
- `AsyncWaiterAwaitable<void>`

must remain separate. They model coroutine synchronization, mailbox delivery, and lifecycle waiting, not socket IO choreography. Forcing them into `AwaitableBuilder` would weaken the abstraction instead of simplifying it.

### 4. Error semantics remain protocol-local

The cutover changes the orchestration mechanism, not protocol error contracts.

Expected external behavior stays the same:

- HTTP paths still expose `HttpError`
- WebSocket paths still expose `WsError`
- HTTP/2 paths still expose `Http2Error` or `Http2ErrorCode`
- SSL paths still expose `SslError`

The internal change is that the mapping from `IOError` / `SslError` into protocol errors should move into flow or machine handlers instead of being duplicated across many `handleComplete()` implementations.

## Architecture

### TCP side

Use `AwaitableBuilder` whenever the protocol flow is linear and can be described as:

1. wait for socket IO
2. inspect IO result
3. parse or mutate protocol state
4. either complete or re-arm another IO step

For simple read/write pipelines, use builder chains:

- `recv(...).parse(...).send(...).finish(...).build()`
- `readv(...).parse(...).writev(...).finish(...).build()`

For flows with more explicit phase transitions or connect-driven setup, use:

- `AwaitableBuilder<ResultT>::fromStateMachine(...).build()`

### SSL side

Use `SslAwaitableBuilder` for SSL transport flows so the protocol layer no longer hand-drives `SslRecvAwaitable` or `SslSendAwaitable` as nested task queues.

This includes flows that currently rely on compatibility glue such as `SslRecvCompatAwaitable`. If a call site can be expressed directly through SSL builder steps, it should be migrated. Compatibility glue is only acceptable when a call site is still in transition and there is no cleaner builder expression yet.

### Public API shape

After cutover, protocol entry points should prefer one of these shapes:

- `auto foo(...)` returning a state-machine awaitable directly
- a named function whose concrete return type is the shared state-machine family

They should not expose a `galay-http`-specific awaitable class name just to preserve an implementation detail.

## Migration Boundaries

### In scope

The cutover targets public or protocol-facing awaitables in:

- `galay-http/kernel/http/HttpReader.h`
- `galay-http/kernel/http/HttpWriter.h`
- `galay-http/kernel/http/HttpSession.h`
- `galay-http/kernel/websocket/WsClient.h`
- `galay-http/kernel/websocket/WsSession.h`
- `galay-http/kernel/websocket/WsReader.h`
- `galay-http/kernel/websocket/WsWriter.h`
- `galay-http/kernel/http2/Http2Conn.h`
- `galay-http/kernel/http2/H2cClient.h`
- `galay-http/kernel/http2/H2Client.h`

### Out of scope

The following stay as they are unless a later change introduces a better non-builder abstraction:

- mailbox delivery awaitables
- waiter/lifecycle awaitables
- scheduler-only helper awaitables that do not model socket IO progress

## Migration Order

To keep the tree buildable and failures local, the cutover should proceed in this order:

1. Update surface tests so they assert the new state-machine awaitable families instead of old custom names.
2. Migrate single-direction write flows.
3. Migrate read/parse/rearm flows.
4. Migrate multi-phase request/upgrade/connect flows.
5. Remove now-dead custom awaitable classes and compat wrappers that are no longer referenced.

Recommended file grouping:

1. HTTP / WebSocket writers
2. HTTP / WebSocket readers
3. HTTP/2 frame readers and writers
4. HTTP session and upgrade/connect flows
5. cleanup and documentation updates

## Testing Strategy

Testing must cover both surface and behavior.

### Surface coverage

Add or update compile-time tests to lock these expectations:

- protocol APIs no longer return `galay-http` custom awaitable names
- socket-driven TCP APIs return the shared kernel state-machine awaitable family
- socket-driven SSL APIs return the shared SSL state-machine awaitable family
- mailbox/waiter APIs remain unchanged where they are intentionally outside the cutover

### Behavior coverage

Keep existing behavioral guarantees green:

- `await_ready()` fast paths for buffered reads
- `await_suspend()` registration behavior
- `await_resume()` result and error mapping
- `timeout(...)` support
- half-packet / need-more re-arm loops
- readv/writev scatter-gather semantics

## Acceptance Criteria

The cutover is complete when all of the following are true:

1. `galay-http` protocol-facing socket IO APIs no longer expose custom `CustomAwaitable` type names.
2. Socket-driven TCP flows are implemented through `StateMachineAwaitable` / `AwaitableBuilder`.
3. Socket-driven SSL flows are implemented through `SslStateMachineAwaitable` / `SslAwaitableBuilder`.
4. Mailbox/waiter awaitables remain explicit exceptions rather than accidental leftovers.
5. Existing awaitable behavior tests and new surface coverage tests pass.
6. Any remaining hand-written protocol `CustomAwaitable` must be documented as an intentional exception with a concrete reason.
