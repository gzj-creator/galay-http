# Epoll Accept Rearm Design

**Date:** 2026-03-07

**Goal:** Restore a trustworthy Linux TCP benchmark path by fixing repeated `co_await accept()` on the same listener under `EpollScheduler`.

**Scope:** This design only covers the Linux `epoll` accept re-arm bug affecting `B2/B3` and `B11/B12`. It does not change `kqueue`, `io_uring`, public `TcpSocket` APIs, or broader `recv/send/readv/writev` optimization work.

## Background

Remote Linux verification showed that the TCP benchmarks were not failing due to port conflicts. The server successfully bound and listened, accepted connections, then started reporting:

- `Accept failed: {} Failed to accept connection (sys: File exists)`

At the same time, the client finished with zero completed requests. That makes the current `B2/B3` benchmark path unreliable as a baseline for further TCP optimization.

Code inspection shows:

1. `AcceptAwaitable::await_suspend()` stores the awaitable on the listener `IOController` and calls `EpollScheduler::addAccept()`.
2. `EpollScheduler::addAccept()` only performs `EPOLL_CTL_ADD`.
3. After an accept completion, the scheduler syncs current epoll interests before `await_resume()` clears `ACCEPT` from the controller.
4. On the next `co_await accept()`, the listener may still already be present in the epoll set, so `EPOLL_CTL_ADD` returns `EEXIST`.

This matches the observed remote failure exactly.

## Problem Statement

Under Linux `epoll`, the same listening socket cannot reliably execute repeated `co_await accept()` cycles because `addAccept()` assumes the listener is not already registered in epoll.

The direct consequence is:

- the benchmark accept loop breaks intermittently or continuously
- Linux TCP benchmark numbers are invalid
- subsequent TCP optimization would be measured against a broken path

## Approaches Considered

### A. Minimal scheduler fix in `EpollScheduler::addAccept()`

Change `addAccept()` to match `addRecv()` / `addSend()`:

- try `EPOLL_CTL_MOD` first
- if `ENOENT`, fall back to `EPOLL_CTL_ADD`

Pros:

- fixes the concrete bug with minimal surface area
- keeps awaitable lifetime and public APIs unchanged
- restores `B2/B3` and `B11/B12` benchmark usability quickly

Cons:

- does not fully clean up the deeper mismatch between epoll registration lifetime and awaitable lifetime

### B. Rework epoll registration lifetime

When `await_resume()` clears the last read or write awaitable, immediately synchronize epoll state or remove the fd from epoll.

Pros:

- semantically cleaner
- eliminates residual registrations when no awaitable is active

Cons:

- larger cross-cutting change
- touches scheduler and awaitable lifetime coupling
- unnecessary risk for this benchmark-restoration step

### C. Patch benchmarks only

Rewrite benchmark accept loops to avoid repeated listener re-arm in the current style.

Pros:

- narrowest change in benchmark code

Cons:

- hides a kernel/runtime bug instead of fixing it
- leaves production listener behavior inconsistent

## Chosen Approach

Choose **A. Minimal scheduler fix in `EpollScheduler::addAccept()`**.

This is the right scope for the current goal because the immediate need is to recover a valid Linux TCP benchmark path. Once the benchmark is trustworthy again, further TCP optimization can be measured against real data instead of a broken accept path.

## Design

### Runtime Behavior

For Linux `epoll` only:

1. `addAccept()` keeps the existing fast path that tries synchronous completion first.
2. If the listener still needs readiness notification, registration should behave like other epoll operations:
   - first try `EPOLL_CTL_MOD`
   - if that fails with `ENOENT`, try `EPOLL_CTL_ADD`
3. No benchmark logic changes are required for correctness.

### Testing Strategy

Add a Linux-only regression test that:

1. creates an `EpollScheduler`
2. creates a listener on loopback
3. performs multiple client connections
4. performs repeated `co_await accept()` on the same listener
5. fails if any accept returns `EEXIST` or if the accept loop stalls

This test should fail before the code change and pass after it.

Then run benchmark smoke verification:

- `B2/B3` should produce non-zero requests
- `B11/B12` should establish connections and exchange data

## Out of Scope

- accept batching
- `readv/writev` awaitable allocation reduction
- `recv/send` coroutine scheduling changes
- `connect` path normalization
- `kqueue` and `io_uring` lifecycle convergence

## Success Criteria

- repeated `co_await accept()` on the same listener works under `EpollScheduler`
- no more Linux benchmark `Accept failed ... File exists`
- `B2/B3` and `B11/B12` become usable again for TCP performance comparison
