# Lightweight Task Core Design

**Date:** 2026-03-07

**Goal:** Replace the current `shared_ptr<CoroutineData>`-based coroutine runtime hot path in `galay-kernel` with a lightweight task core so same-thread wakeups and ready-queue operations stop paying wrapper copies and shared ownership overhead.

**Scope:** This design covers the internal task representation used by `Coroutine`, `Waker`, `Scheduler`, `EpollScheduler`, `KqueueScheduler`, and `IOUringScheduler`. Public coroutine APIs stay stable in this phase. Reactor semantics, channel public APIs, and TCP surface APIs are out of scope unless required to preserve correctness.

## Background

Recent remote `Release + epoll` measurements show two distinct facts:

1. `readv/writev` has already been flattened substantially and now trails plain `send/recv` by about 8%.
2. The larger remaining gap is inside the runtime wake path itself.

Current wake flow:

1. awaitable completion calls `awaitable->m_waker.wakeUp()`
2. `Waker::wakeUp()` calls `Coroutine::resume()`
3. `Coroutine::resume()` does `m_queued.exchange(true)` and `scheduler->spawn(*this)`
4. the scheduler stores heavyweight `Coroutine` wrappers in ready structures
5. the worker later pops the wrapper and finally resumes the underlying handle

This means same-thread completion still pays for:

- `Waker` holding a full `Coroutine`
- `Coroutine` holding a `shared_ptr<CoroutineData>`
- shared ownership refcount traffic
- `Coroutine` object moves through `optional/deque/ConcurrentQueue`
- an extra `resume -> spawn -> queue -> pop -> Scheduler::resume` chain

That model is materially heavier than Tokio's task-pointer-oriented wake path.

## Problem Statement

The current task representation has three structural issues:

1. **Task identity is too heavy**
   - `Coroutine` is used both as the user-visible coroutine value and as the scheduler's internal ready-task representation.
   - Every copy/move drags shared ownership machinery into the hot path.
2. **Wakeup indirection is too deep**
   - `Waker` cannot schedule a task directly; it must rebuild scheduling through `Coroutine::resume()`.
3. **Ready queues carry wrapper objects**
   - worker-local queues and injected queues move `Coroutine` objects rather than a compact task reference.

As long as these three stay coupled, scheduler-local improvements can only recover part of the remaining gap.

## Chosen Approach

Adopt a two-layer runtime model:

- **Public layer:** `Coroutine`
  - remains the type users return, spawn, and wait on
  - becomes a thin handle/view over an internal task core
- **Runtime layer:** `TaskState` + `TaskRef`
  - `TaskState` owns the coroutine handle and scheduler/runtime state
  - `TaskRef` is the lightweight schedulable reference used by `Waker` and worker queues

### Why this approach

Three options were considered:

### A. Keep `Coroutine` as-is and only special-case `Waker`

Pros:
- Smallest diff
- Lower risk

Cons:
- Leaves `shared_ptr` and wrapper-heavy queues in the hot path
- Cannot remove the main structural cost

### B. Introduce a lightweight task core behind stable APIs

Pros:
- Attacks the real bottleneck directly
- Preserves user-facing coroutine APIs
- Creates a reusable base for later scheduler and reactor work

Cons:
- Requires careful lifecycle and waiter-chain migration
- Touches all scheduler backends

### C. Full Tokio-style reactor/task rewrite now

Pros:
- Maximum long-term alignment

Cons:
- Too much surface area for one step
- Hard to isolate regressions and benchmark wins

**Chosen:** B.

## Architecture

### TaskState

`TaskState` becomes the single runtime record per coroutine:

- `std::coroutine_handle<Coroutine::promise_type> handle`
- `Scheduler* scheduler`
- `std::atomic<uint32_t> ref_count`
- `std::atomic<bool> queued`
- `std::atomic<bool> done`
- waiter continuation storage for `co_await coro.wait()`

`TaskState` lifetime is no longer managed by `std::shared_ptr`. Instead it uses intrusive retain/release controlled by:

- `Coroutine`
- `Waker`
- ready-queue entries
- waiter linkage where necessary

### TaskRef

`TaskRef` is the runtime scheduling token:

- stores `TaskState*`
- retains/releases intrusive refcount
- provides cheap move/copy compared with `Coroutine`
- can expose helpers such as `scheduler()`, `handle()`, `done()`, `markQueued()`

Worker structures store `TaskRef`, not `Coroutine`.

### Coroutine

`Coroutine` remains the public type but becomes a thin owner/reference to `TaskState`.

Responsibilities:

- construct `TaskState` from promise
- expose `belongScheduler`, `done`, `wait`
- convert to `TaskRef` for runtime paths

It should stop being the scheduler's ready-queue currency.

### Waker

`Waker` stores `TaskRef` directly.

New hot path:

1. awaitable captures `Waker(TaskRef)`
2. completion calls `waker.wakeUp()`
3. `Waker` directly calls scheduler runtime scheduling with `TaskRef`
4. worker queues receive `TaskRef`
5. worker drains and resumes handle

This removes the `Waker -> Coroutine::resume -> spawn(*this)` wrapper round-trip.

### Scheduler integration

Add an internal scheduler entrypoint for ready tasks, for example:

- `scheduleReady(TaskRef task)`
- `scheduleReadyDeferred(TaskRef task)` if needed for yield/fairness semantics

`spawn(Coroutine)` remains public API and simply lowers to the internal task representation.

## Lifecycle and correctness rules

The tricky parts are:

1. **final suspend / completion**
   - task must mark `done`
   - waiter continuation must still resume exactly once
   - runtime refs must release correctly after final resume
2. **wait chaining**
   - `co_await coro.wait()` currently stores `m_next`
   - this must migrate to a lightweight continuation link that does not require copying a full `Coroutine`
3. **queue ownership**
   - ready queue push/pop must not leak or prematurely destroy task state
4. **cross-backend parity**
   - `epoll`, `kqueue`, and `io_uring` must all use the same runtime task rules even if wake plumbing differs

## Testing strategy

This phase should be validated with a mix of semantics tests and hot-path regression tests:

- new runtime task-core test:
  - same-thread wake resumes once
  - waiter chaining still resumes once
  - repeated wake attempts do not double-enqueue
- existing scheduler behavior tests:
  - `T24-Spawn`
  - `T25-SpawnSimple`
  - `T26-ConcurrentRecvSend`
  - `T31-UnsafeChannelDeferredWake`
  - `T32-IOSchedulerLocalFirst`
- existing IO regression tests:
  - `T19-ReadvWritev`
  - `T20-RingbufferIo`
  - `T34-ReadvArrayBorrowed`
  - `T35-ReadvArrayCountGuard`
- performance verification:
  - `B8-MpscChannel`
  - `B9-UnsafeChannel`
  - `B2/B3`
  - `B11/B12`

## Non-goals for this phase

- replace `ConcurrentQueue` inject path
- redesign `epoll/kqueue/io_uring` completion dispatch
- introduce user-visible new awaitable APIs
- inline-resume everything on same-thread wake

The point of this phase is narrower: make the runtime carry lightweight task references first, then measure again before touching reactor structure.
