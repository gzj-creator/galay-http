# galay-http Full Awaitable Rollout Design

**Date:** 2026-03-21

## Goal

Complete the `galay-http` awaitable cutover so the repository no longer depends on protocol-local socket IO awaitable scaffolding where `galay-kernel` `AwaitableBuilder` / `StateMachineAwaitable` or `galay-ssl` `SslAwaitableBuilder` / `SslStateMachineAwaitable` can express the same flow more clearly, then verify the entire repository through fresh examples, tests, and benchmarks.

After `galay-http` is stable, reuse the same migration and verification playbook for the remaining `galay-*` repositories.

## Confirmed Scope

This round is not limited to one protocol family. The required end state is:

- `galay-http` protocol-facing socket flows are fully converged
- dead or redundant custom awaitable classes and compatibility glue are removed
- outdated tests that only lock obsolete concrete awaitable names are rewritten or removed
- all repository examples, tests, and benchmarks pass in a fresh validation run
- only then does work continue to other repositories

## Design Decisions

### 1. Shared awaitable families are the default orchestration layer

For protocol socket IO flows:

- TCP paths should use `AwaitableBuilder` or `StateMachineAwaitable`
- SSL paths should use `SslAwaitableBuilder` or `SslStateMachineAwaitable`

`galay-http` should not keep protocol-local awaitable class names merely to preserve historical implementation details.

### 2. Socket IO flows and coordination awaitables stay intentionally separated

The repository still contains awaitables that are not socket choreography:

- mailbox delivery awaitables
- lifecycle shutdown awaitables
- waiter-based completion awaitables

These remain valid exceptions if they model coroutine coordination rather than transport progress.

### 3. The migration is accepted only when repository-level verification is green

Local compile success is not enough. The cutover is only considered complete when:

- examples build and run
- tests pass
- benchmarks build and complete without new crashes, deadlocks, or obvious regressions

Any protocol path that cannot survive full-repository verification is not considered done.

### 4. Surface simplification is required

The repository should continue moving toward:

- fewer protocol-local awaitable class names
- fewer compatibility wrappers
- smaller public surface tests focused on behavioral families rather than historical concrete types

If a test exists only to pin an obsolete implementation detail, it should be deleted or rewritten.

## Migration Strategy

### Phase 1: Stabilize public surface expectations

Update the surface tests so they assert:

- socket-facing HTTP / WS / H2 / H2c / WSS entry points return the shared awaitable families
- explicit coordination awaitables remain allowed exceptions

This gives the rest of the migration a stable target.

### Phase 2: Finish HTTP and WebSocket cutover

Fully converge:

- `HttpReader`
- `HttpWriter`
- `HttpSession`
- `WsReader`
- `WsWriter`
- `WsSession`
- `WsClient`

At the end of this phase, HTTP/1.1 and WebSocket should no longer rely on protocol-local socket awaitable orchestration.

### Phase 3: Finish HTTP/2 and TLS-adjacent cutover

Converge:

- `Http2Conn`
- `H2cClient`
- `H2Client`
- `Http2Server`
- `Http2StreamManager`

Retain only the coordination-specific awaitables that are still semantically justified.

### Phase 4: Clean dead code and test debt

Remove:

- obsolete compat wrappers
- dead custom awaitable helpers
- tests with no remaining value after the new surface is locked

Keep only tests that meaningfully cover surface, behavior, lifecycle, and protocol semantics.

### Phase 5: Fresh repository verification

Run a full verification pass for:

- examples
- tests
- benchmarks

If verification exposes unstable cases, fix them before declaring the migration complete.

### Phase 6: Cross-repo continuation

After `galay-http` is clean and verified:

- continue `galay-redis`
- continue `galay-etcd`
- continue `galay-mysql`
- then any remaining `galay-*` repositories that still depend on old awaitable patterns

The same rule applies there: migration is not done until regression and performance verification are green.

## Acceptance Criteria

`galay-http` is complete only when all of the following are true:

1. Protocol-facing socket flows use shared kernel / SSL awaitable families.
2. No redundant protocol-local socket awaitable scaffolding remains.
3. Surface tests reflect the new contract rather than obsolete type names.
4. The full repository example suite passes.
5. The full repository test suite passes.
6. The benchmark suite builds and runs without new failures.
7. Remaining non-builder awaitables are explicit, justified exceptions.
8. The repository is ready for the same rollout process to continue in the remaining `galay-*` repositories.
