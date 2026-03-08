# IO Scheduler Worker Redesign Design

**Date:** 2026-03-07

**Goal:** Reduce fixed scheduling overhead in `galay-kernel` by redesigning `epoll` and `kqueue` IO scheduler worker execution around local-first task scheduling before undertaking a deeper readiness/reactor rewrite.

**Scope:** This design covers `EpollScheduler` and `KqueueScheduler` only. `ComputeScheduler`, public awaitable APIs, `TcpSocket` surface APIs, and `io_uring` are intentionally out of scope for this phase.

## Background

Current `galay-http` `h2c` throughput is materially below the Rust/Tokio baseline under small-payload, high-concurrency workloads. Recent measurements show the C++ stack ahead of Go but still substantially behind Rust. Prior micro-optimizations in HTTP/2 batching, borrowed `iovec` handling, and channel wake strategy only produced sub-1% improvements, which indicates the remaining gap is dominated by deeper runtime costs.

The current `galay-kernel` wake path for IO completion is structurally expensive:

1. IO completion invokes `awaitable->m_waker.wakeUp()`.
2. `Waker::wakeUp()` calls `Coroutine::resume()`.
3. `Coroutine::resume()` re-enqueues the coroutine through `scheduler->spawn()`.
4. The scheduler later drains the shared queue and finally resumes the coroutine.

This means even same-thread, same-worker wakeups pay the cost of queueing and a delayed resumption pass. That is particularly harmful for protocols like HTTP/2 where reader, writer, and active-handler coroutines frequently wake one another in short ping-pong chains.

## Problem Statement

The current `EpollScheduler` and `KqueueScheduler` share three structural issues:

- They use a single shared coroutine queue as the dominant execution path.
- Same-thread wakeups are not treated specially, so locality is wasted.
- Cross-thread submission and same-thread resumption are not separated cleanly in the worker model.

Additionally, `KqueueScheduler::spawn()` currently does not explicitly notify the parked worker on cross-thread submission, unlike `EpollScheduler`, which leaves avoidable wake latency in the macOS path.

## Chosen Approach

This phase adopts a Tokio-inspired worker scheduling model for `epoll` and `kqueue`, without yet changing the IO registration model.

### Key Idea

Replace the current “completion -> shared queue -> later resume” hot path with a layered local-first worker scheduler:

1. `lifo_slot`
   - Single fast slot for the most recently scheduled same-worker coroutine.
   - Optimizes short request-response or channel ping-pong patterns.
2. `local_queue`
   - Worker-owned queue used for same-thread scheduling when `lifo_slot` is occupied.
3. `inject_queue`
   - Cross-thread submission path.
   - Used by external threads, overflow from `local_queue`, and future steal redistribution.
4. `steal`
   - Idle workers may steal from peers before parking.

This does **not** attempt to redesign IO readiness registration in this phase. The goal is to first remove unnecessary scheduler indirection and recover locality.

## Why This Approach

Three approaches were considered:

### A. Readiness/reactor rewrite first

Pros:
- Targets the largest long-term reactor overhead.
- Aligns more directly with Tokio's `Registration` / `ScheduledIo` model.

Cons:
- Much larger surface area.
- Harder to isolate regressions.
- Would obscure whether scheduler locality alone can recover a meaningful portion of the current gap.

### B. Scheduler redesign first

Pros:
- Directly attacks a confirmed current cost: wake-to-resume indirection.
- Keeps public APIs stable.
- Makes later readiness work cleaner because the worker execution model becomes more disciplined first.

Cons:
- Will not eliminate per-operation IO registration overhead.
- May not close the entire Rust gap by itself.

### C. Backend-specific hot patches only

Pros:
- Lowest initial code churn.
- Fastest path to a benchmark result.

Cons:
- Risks accumulating one-off optimizations.
- Does not establish a reusable worker model.

**Chosen:** B. Scheduler redesign first.

## Architecture

### Worker State

Each `IOScheduler` worker will maintain explicit local execution state:

- `lifo_slot`
  - Optional single coroutine scheduled to run next.
- `local_queue`
  - Worker-local FIFO queue for same-thread scheduling.
- `inject_queue`
  - Cross-thread, wake-capable queue for external submissions.
- `remote_handles`
  - Peer-facing handles used for cross-thread injection and future stealing.
- fairness counters
  - Used to cap consecutive `lifo_slot` execution and force periodic global/injected work checks.

`epoll` and `kqueue` should share this model conceptually, even if implementation details differ because `epoll` already has `eventfd`-based wakeup while `kqueue` uses a pipe.

### Scheduling Rules

#### Same-thread scheduling

When a coroutine is scheduled from the worker thread that owns it:

- First choice: place it in `lifo_slot` if empty.
- Fallback: push it into `local_queue`.
- No OS wake notification is sent.

This covers the common case where an IO completion or internal channel wake occurs on the same worker thread.

#### Cross-thread scheduling

When a coroutine is scheduled from a different thread:

- Push it into `inject_queue`.
- Notify/unpark the target worker immediately.

This rule applies uniformly to both `epoll` and `kqueue`.

#### Worker run order

The worker event loop should prioritize work in this order:

1. `lifo_slot`
2. `local_queue`
3. periodic `inject_queue` drain
4. work stealing from peers
5. park in `epoll_wait` / `kevent`

This preserves locality while keeping fairness and global progress.

### Fairness

A pure LIFO model can starve older tasks under ping-pong workloads. To prevent that:

- Cap consecutive `lifo_slot` polls per tick.
- Force periodic checks of `inject_queue` even when local work remains.
- On cap hit, demote the pending LIFO coroutine into `local_queue`.

This should produce a hybrid model: locality when beneficial, fairness when needed.

### Wake Path Changes

Current wake path:

- completion -> `Waker::wakeUp()` -> `Coroutine::resume()` -> `spawn()` -> shared queue -> batch drain -> `resume`

Target wake path for same-worker cases:

- completion -> `Waker::wakeUp()` -> scheduler-local fast schedule -> `lifo_slot` / `local_queue` -> immediate next worker iteration

The design intentionally keeps `Waker` and public coroutine contracts stable from the caller's perspective. The semantic change is internal: same-thread scheduling stops paying the full shared-queue path by default.

## File-Level Impact

Expected primary files:

- `../galay-kernel/galay-kernel/kernel/KqueueScheduler.h`
- `../galay-kernel/galay-kernel/kernel/KqueueScheduler.cc`
- `../galay-kernel/galay-kernel/kernel/EpollScheduler.h`
- `../galay-kernel/galay-kernel/kernel/EpollScheduler.cc`
- `../galay-kernel/galay-kernel/kernel/Coroutine.h`
- `../galay-kernel/galay-kernel/kernel/Coroutine.cc`
- Possibly shared scheduler support headers if the worker-state abstraction is extracted.

Secondary files:

- `../galay-kernel/test/T*.cc` for new scheduler correctness tests.
- `../galay-kernel/test/CMakeLists.txt` should not require changes because it auto-discovers `T*.cc` tests.

## Invariants

The redesign must preserve these invariants:

- No duplicate queueing of the same coroutine.
- No lost wakeups across same-thread or cross-thread submissions.
- No starvation caused by the LIFO fast path.
- No behavior change for public `spawn`, `co_await spawn(...)`, and `Coroutine.wait()` semantics.
- `epoll` and `kqueue` must both support explicit worker wakeup on cross-thread injection.

## Testing Strategy

### Kernel correctness tests

Add focused scheduler tests that prove:

- same-thread wake prefers LIFO execution order
- LIFO demotes correctly after the configured cap
- cross-thread injection wakes the parked worker immediately
- injected and local work are both eventually executed
- worker fairness prevents permanent starvation

### HTTP/2 regression tests

Reuse existing `galay-http` coverage after kernel changes:

- `T41-H2CloseTcpTeardown`
- `T42-H2ActiveConnApi`
- `T43-H2ActiveConnPreferred`
- `T44-H2cClientShutdown`
- `T45-H2AwaitableSurface`
- `T46-H2ActiveConnRetire`
- `T47-IoVecBorrowCursor`
- `T25-H2cClient` against a locally started `B3-H2cServer`

### Performance checks

Use the existing `h2c` comparison flow to evaluate whether this scheduler redesign recovers throughput from the current ~73k RPS region toward or above the prior ~83.5k RPS baseline before any readiness/reactor rewrite.

## Rollout Plan

1. Implement the worker model for `kqueue` first.
2. Verify correctness and regressions on macOS.
3. Port the same worker model to `epoll`.
4. Re-run kernel tests and HTTP/2 regressions.
5. Re-benchmark `h2c`.
6. Only then decide whether the next phase should be readiness/reactor convergence.

## Non-Goals

This phase explicitly does not:

- redesign IO registration around long-lived readiness state
- change public socket awaitable signatures
- optimize `io_uring`
- refactor `ComputeScheduler`
- bundle unrelated HTTP/2 refactors into the runtime change

## Success Criteria

This phase is successful if:

- `epoll` and `kqueue` both run on the new local-first worker model
- kernel scheduler correctness tests pass
- existing HTTP/2 regressions still pass
- `h2c` throughput improves materially over the current baseline
- the design leaves a clean next step for a future readiness/reactor rewrite
