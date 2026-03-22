# galay-http Remove Coroutine Design

**Date:** 2026-03-22

## Goal

Remove `Coroutine` completely from `galay-http`, including repository-local compatibility headers, public APIs, internal orchestration code, tests, examples, benchmarks, and user-facing documentation, while keeping the repository readable, performant, and fully verified through fresh test, example, and benchmark runs.

## Confirmed Scope

This round is a deliberate breaking change.

The required end state is:

- repository code no longer defines, includes, or exposes `Coroutine`
- downstream users implement async handlers as `Task<void>` instead of custom `Coroutine` wrappers
- internal long-running flows also use `Task<void>`
- coordination primitives such as kernel-native socket awaitables, timeout helpers, and `AsyncWaiter` remain allowed where they are not `Coroutine` compatibility layers
- all repository tests, examples, and benchmarks pass after the migration

## Current State

The system-installed `galay-kernel` in `/usr/local/include/galay-kernel` is already `Task`-based and no longer provides `Coroutine`.

The remaining `Coroutine` dependency comes from this repository:

- `galay-kernel/kernel/Coroutine.h` reintroduces a compatibility layer
- `galay-kernel/kernel/Runtime.h` pulls that compatibility layer back into the include graph
- `galay-http` public headers still expose `std::function<Coroutine(...)>` and `Coroutine` return types
- many tests, examples, benchmarks, and docs still teach or depend on the old surface

Because the root CMake setup includes the repository source directory directly, the vendored compatibility layer is part of the active build even though the installed kernel has already moved on.

## Design Decisions

### 1. `Task<void>` becomes the only async function contract

All user-provided handlers and all internal orchestration functions move to `Task<void>`.

This includes:

- HTTP route handlers
- HTTP connection handlers
- HTTP/2 stream handlers
- HTTP/2 active-connection handlers
- HTTP/1 fallback handlers
- internal server loops, reader loops, writer loops, monitor loops, shutdown flows, and helper tasks

No type alias, adapter, or compatibility shim will preserve `Coroutine` as a public or internal contract.

### 2. Serial child-task waiting uses direct `co_await`

Any existing `co_await foo().wait();` pattern will be replaced by direct `co_await foo();`.

This removes an extra compatibility layer and makes control flow easier to read. It also aligns the repository with the kernel-native `Task` semantics instead of emulating join behavior through `Coroutine`.

### 3. Background task orchestration stops depending on `Coroutine::wait()`

The largest migration risk is in long-lived HTTP/2 orchestration paths that currently rely on combinations like:

- `spawn(coroutine)`
- storing `Coroutine writer; Coroutine monitor;`
- later calling `co_await writer.wait();`

That model will be replaced with pure `Task` orchestration:

- start background tasks by scheduling or spawning `Task<void>`
- record lifecycle completion through `AsyncWaiter<void>` or equivalent task-aware signaling
- await completion asynchronously from foreground flows without blocking scheduler threads

`JoinHandle::join()` and `JoinHandle::wait()` remain acceptable only in non-coroutine control flow such as tests or top-level runtime management. They will not be used inside coroutine tasks.

### 4. Waiter and mailbox wakeups become task-generic

Any code that currently hard-binds to `Coroutine::promise_type` must be rewritten around task-generic primitives such as:

- `taskRefView()`
- `TaskRef`
- `Waker`

This is especially important for HTTP/2 mailbox and coordination code that currently stores `std::coroutine_handle<Coroutine::promise_type>`.

The replacement should preserve wakeup semantics while removing direct type coupling to the deleted compatibility layer.

### 5. Kernel-native awaitables stay where they model real IO or coordination

This migration removes `Coroutine`, not the entire awaitable vocabulary.

The following remain valid when they represent actual behavior instead of `Coroutine` compatibility:

- socket `connect/recv/send/close` awaitables from `galay-kernel`
- timeout-capable operation wrappers
- `AsyncWaiter`-based coordination awaitables
- existing protocol operations that are already native awaitables and do not require users to implement custom compatibility classes

This keeps the migration focused and avoids unnecessary churn in stable low-level primitives.

## Migration Strategy

### Phase 1: Delete the repository-local compatibility root

Remove:

- `galay-kernel/kernel/Coroutine.h`

Update:

- `galay-kernel/kernel/Runtime.h` so it only forwards to the installed runtime header

After this phase, any remaining `Coroutine` reference becomes a compile-time migration target instead of silently continuing through the vendored shim.

### Phase 2: Convert public `galay-http` signatures

Update all public headers so handler and loop signatures are `Task<void>`-based.

This intentionally breaks downstream source compatibility and makes the new contract explicit at compile time.

### Phase 3: Convert internal orchestration

Update internal implementations to:

- use `Task<void>` returns
- replace `.wait()` chains with direct `co_await`
- replace stored background `Coroutine` objects with task scheduling plus waiter-based completion signaling

The highest-risk area is `Http2StreamManager`, followed by HTTP/2 server/client control paths.

### Phase 4: Convert repository consumers

Update:

- tests
- examples
- benchmarks
- docs

Any sample or regression that still references `Coroutine` will be migrated to `Task<void>` or removed if it exists only to preserve deleted compatibility behavior.

### Phase 5: Full verification and commit gate

The code changes are not considered complete until:

- all tests pass
- all examples pass
- all benchmarks build and run successfully

Only after fresh verification is green should the implementation be committed as the migration version the user asked for.

## Error Handling and Performance Constraints

Error mapping should remain behaviorally stable unless a `Coroutine` compatibility artifact forced a change.

In particular:

- `IOError`, `HttpError`, and `Http2Error` surfaces should remain consistent
- shutdown, GOAWAY, peer-close, timeout, and fallback paths should preserve existing ordering semantics
- no scheduler thread may block on `JoinHandle::join()` inside a task

Performance should not regress merely to preserve old orchestration structure:

- direct `co_await Task<void>` is preferred over compatibility wrappers
- background loops must remain event-driven
- unnecessary heap layers or thread handoffs should not be introduced

## Acceptance Criteria

The migration is complete only when all of the following are true:

1. `rg -n "\\bCoroutine\\b" galay-http galay-kernel test examples benchmark docs CMakeLists.txt` returns no active repository usage other than historical plan records or external benchmark artifacts.
2. `galay-kernel/kernel/Coroutine.h` is removed from the repository.
3. `galay-kernel/kernel/Runtime.h` no longer includes `Coroutine.h`.
4. Public `galay-http` APIs require `Task<void>` for async handlers.
5. Internal orchestration no longer depends on `.wait()`, `spawn(Coroutine)`, `scheduleCoroutine(...)`, or `Coroutine::promise_type`.
6. Fresh repository verification passes for tests, examples, and benchmarks.
7. The final committed version is the fully verified one, ready for later Go/Rust same-environment performance comparison.
