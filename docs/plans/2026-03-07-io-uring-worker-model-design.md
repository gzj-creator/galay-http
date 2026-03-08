# IOUring Worker Model Redesign Design

**Context**

`KqueueScheduler` has already been moved to the new local-first worker model, and `EpollScheduler` has been updated symmetrically in the current worktree. `IOUringScheduler` still uses the old shared coroutine queue path, so same-thread wakeups, deferred yields, and cross-thread inject behavior remain inconsistent across backends.

**Goal**

Bring `IOUringScheduler` onto the same worker scheduling model as `kqueue` and `epoll` without changing existing SQE/CQE submission semantics in this batch.

**Scope**

This batch only changes coroutine scheduling on the scheduler side:
- same-thread `spawn()` should prefer local-first execution
- `co_yield true` should use deferred local scheduling rather than preempting already-ready work
- cross-thread `spawn()` should use inject queue + existing `eventfd` wakeup
- existing `io_uring` completion processing, SQE preparation, CQE dispatch, and timeout wiring stay unchanged

**Recommended Approach**

Reuse the already introduced `IOSchedulerWorkerState` and the `Scheduler::spawnDeferred()` split:
- port `IOUringScheduler::spawn()` to `scheduleLocal()` / `scheduleInjected()`
- add `IOUringScheduler::spawnDeferred()` mirroring `kqueue` and `epoll`
- rewrite `processPendingCoroutines()` to pull from `m_worker` rather than `m_coro_queue`
- keep `eventfd` as the wake mechanism for remote injects and parked waits

This keeps the backend-specific part focused on SQE/CQE handling while making scheduler behavior consistent across all three backends.

**Why This Approach**

This is the lowest-risk path:
- no change to completion semantics
- no change to socket/file awaitable APIs
- minimal diff surface in `IOUringScheduler`
- allows direct reuse of `T32` scheduler behavior expectations once Linux validation is available

**Testing Strategy**

Primary guard:
- extend `T32-IOSchedulerLocalFirst` so it can also target `IOUringScheduler` when `USE_IOURING` is defined

Local limitation:
- current machine is macOS, so `io_uring` cannot be compiled or executed here
- implementation will therefore be done with static symmetry checks against the validated `kqueue/epoll` paths

Linux validation should run:
- `T17-UnsafeChannel`
- `T24-Spawn`
- `T25-SpawnSimple`
- `T26-ConcurrentRecvSend`
- `T31-UnsafeChannelDeferredWake`
- `T32-IOSchedulerLocalFirst`

**Success Criteria**

- `IOUringScheduler` no longer depends on its old shared coroutine queue path
- local-first / deferred-yield / inject behavior matches `kqueue` and `epoll`
- Linux `USE_IOURING` builds pass `T32` and the key regression set above
