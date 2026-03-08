# Lightweight Task Core Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace `shared_ptr<CoroutineData>` runtime scheduling with a lightweight task core while keeping public coroutine APIs stable.

**Architecture:** Introduce intrusive `TaskState` ownership and a lightweight `TaskRef` used by `Waker` and scheduler ready queues. Keep `Coroutine` as the public surface, but lower scheduler/runtime paths onto `TaskRef` so same-thread wakeups stop moving heavyweight wrapper objects through the hot path.

**Tech Stack:** C++20 coroutines, intrusive refcounting, existing `galay-kernel` schedulers (`epoll`, `kqueue`, `io_uring`), existing runtime tests and benchmarks.

---

### Task 1: Add a failing runtime test that locks down lightweight task-core semantics

**Files:**
- Create: `../galay-kernel/test/T36-TaskCoreWakePath.cc`
- Modify: `../galay-kernel/test/CMakeLists.txt`

**Step 1: Write the failing test**

Create `T36-TaskCoreWakePath.cc` covering:
- same-thread wake resumes a suspended task exactly once
- repeated `wakeUp()` before drain does not double-run the task
- `co_await child.wait()` still resumes the waiter exactly once after child completion

Suggested test structure:

```cpp
struct WakeState {
    std::atomic<int> resumed{0};
    std::atomic<bool> waiter_done{false};
};

Coroutine waiter(WakeState* state, AsyncWaiter<void>* gate);
Coroutine child(WakeState* state);
bool runSameThreadSingleResume();
bool runWaitChainResumeOnce();
```

**Step 2: Run test to verify it fails**

Run:

```bash
cmake --build ../galay-kernel/build --target T36-TaskCoreWakePath -j4
./../galay-kernel/build/test/T36-TaskCoreWakePath
```

Expected: build failure or runtime failure because the test target and task-core semantics do not exist yet.

**Step 3: Write minimal implementation**

Only add the new test target and the smallest runtime probes/assertions needed to expose the current heavyweight wake path limitations.

**Step 4: Run test again to verify the failure is meaningful**

Run:

```bash
./../galay-kernel/build/test/T36-TaskCoreWakePath
```

Expected: FAIL for the intended reason, not for typos or unrelated setup errors.

**Step 5: Commit**

```bash
git add ../galay-kernel/test/T36-TaskCoreWakePath.cc ../galay-kernel/test/CMakeLists.txt
git commit -m "test: add task core wake path regression coverage"
```

### Task 2: Introduce `TaskState` and `TaskRef` without changing public `Coroutine` APIs

**Files:**
- Modify: `../galay-kernel/galay-kernel/kernel/Coroutine.h`
- Modify: `../galay-kernel/galay-kernel/kernel/Coroutine.cc`
- Test: `../galay-kernel/test/T36-TaskCoreWakePath.cc`

**Step 1: Extend the failing test if needed**

If the new task model requires explicit assertions for retain/release safety or waiter completion ordering, add them to `T36-TaskCoreWakePath.cc` first.

**Step 2: Run test to verify it still fails**

Run:

```bash
cmake --build ../galay-kernel/build --target T36-TaskCoreWakePath -j4
./../galay-kernel/build/test/T36-TaskCoreWakePath
```

Expected: FAIL because `Coroutine` still uses `shared_ptr<CoroutineData>`.

**Step 3: Write minimal implementation**

Refactor coroutine core types:
- add intrusive `TaskState`
- add lightweight `TaskRef`
- make `Coroutine` hold the new task core instead of `std::shared_ptr<CoroutineData>`
- preserve public methods:
  - `done()`
  - `wait()`
  - `belongScheduler()`
  - `resume()` only if still required temporarily

Do not update schedulers yet beyond what is strictly necessary to compile.

**Step 4: Run test to verify partial behavior**

Run:

```bash
./../galay-kernel/build/test/T36-TaskCoreWakePath
```

Expected: test may still fail on wake scheduling, but core task state should compile and basic lifecycle assertions should move forward.

**Step 5: Commit**

```bash
git add ../galay-kernel/galay-kernel/kernel/Coroutine.h ../galay-kernel/galay-kernel/kernel/Coroutine.cc ../galay-kernel/test/T36-TaskCoreWakePath.cc
git commit -m "refactor: introduce lightweight task core"
```

### Task 3: Migrate `Waker` to hold `TaskRef` and schedule ready tasks directly

**Files:**
- Modify: `../galay-kernel/galay-kernel/kernel/Waker.h`
- Modify: `../galay-kernel/galay-kernel/kernel/Waker.cc`
- Modify: `../galay-kernel/galay-kernel/kernel/Scheduler.hpp`
- Modify: `../galay-kernel/galay-kernel/kernel/Coroutine.cc`
- Modify: `../galay-kernel/galay-kernel/kernel/Awaitable.cc`
- Modify: `../galay-kernel/concurrency/MpscChannel.h`
- Modify: `../galay-kernel/concurrency/AsyncWaiter.h`
- Test: `../galay-kernel/test/T36-TaskCoreWakePath.cc`
- Test: `../galay-kernel/test/T31-UnsafeChannelDeferredWake.cc`

**Step 1: Add the failing behavior checks first**

If needed, extend `T36-TaskCoreWakePath.cc` so one assertion specifically proves that multiple wake attempts before drain do not duplicate queue entries.

**Step 2: Run tests to verify they fail**

Run:

```bash
cmake --build ../galay-kernel/build --target T31-UnsafeChannelDeferredWake T36-TaskCoreWakePath -j4
./../galay-kernel/build/test/T31-UnsafeChannelDeferredWake
./../galay-kernel/build/test/T36-TaskCoreWakePath
```

Expected: at least one test fails before `Waker` is flattened.

**Step 3: Write minimal implementation**

Change wake flow to:
- `Waker` stores `TaskRef`, not `Coroutine`
- `wakeUp()` directly routes to an internal scheduler ready-task API
- awaitables/channels/waiters capture the new lightweight wake token

Keep public surface APIs unchanged.

**Step 4: Run tests to verify they pass**

Run:

```bash
./../galay-kernel/build/test/T31-UnsafeChannelDeferredWake
./../galay-kernel/build/test/T36-TaskCoreWakePath
```

Expected: PASS.

**Step 5: Commit**

```bash
git add ../galay-kernel/galay-kernel/kernel/Waker.h ../galay-kernel/galay-kernel/kernel/Waker.cc ../galay-kernel/galay-kernel/kernel/Scheduler.hpp ../galay-kernel/galay-kernel/kernel/Coroutine.cc ../galay-kernel/galay-kernel/kernel/Awaitable.cc ../galay-kernel/galay-kernel/concurrency/MpscChannel.h ../galay-kernel/galay-kernel/concurrency/AsyncWaiter.h ../galay-kernel/test/T31-UnsafeChannelDeferredWake.cc ../galay-kernel/test/T36-TaskCoreWakePath.cc
git commit -m "refactor: route wakeups through lightweight task refs"
```

### Task 4: Replace scheduler ready containers from `Coroutine` to lightweight task refs

**Files:**
- Modify: `../galay-kernel/galay-kernel/kernel/IOScheduler.hpp`
- Modify: `../galay-kernel/galay-kernel/kernel/EpollScheduler.cc`
- Modify: `../galay-kernel/galay-kernel/kernel/KqueueScheduler.cc`
- Modify: `../galay-kernel/galay-kernel/kernel/IOUringScheduler.cc`
- Test: `../galay-kernel/test/T32-IOSchedulerLocalFirst.cc`
- Test: `../galay-kernel/test/T36-TaskCoreWakePath.cc`

**Step 1: Extend failing tests first**

If necessary, add assertions so `T32-IOSchedulerLocalFirst.cc` still verifies:
- same-thread wake runs before older queued work where intended
- cross-thread inject still unparks quickly
- fairness guard still holds

**Step 2: Run tests to verify they fail or are incomplete**

Run:

```bash
cmake --build ../galay-kernel/build --target T32-IOSchedulerLocalFirst T36-TaskCoreWakePath -j4
./../galay-kernel/build/test/T32-IOSchedulerLocalFirst
./../galay-kernel/build/test/T36-TaskCoreWakePath
```

Expected: FAIL or compile break until ready queues stop storing `Coroutine`.

**Step 3: Write minimal implementation**

Update scheduler internals:
- `lifo_slot` stores `TaskRef`
- `local_queue` stores `TaskRef`
- `inject_queue` stores `TaskRef`
- worker drain resumes from `TaskRef` directly
- keep public `spawn(Coroutine)` lowering onto the new internal representation

Do not redesign the inject queue itself yet.

**Step 4: Run tests to verify they pass**

Run:

```bash
./../galay-kernel/build/test/T32-IOSchedulerLocalFirst
./../galay-kernel/build/test/T36-TaskCoreWakePath
```

Expected: PASS.

**Step 5: Commit**

```bash
git add ../galay-kernel/galay-kernel/kernel/IOScheduler.hpp ../galay-kernel/galay-kernel/kernel/EpollScheduler.cc ../galay-kernel/galay-kernel/kernel/KqueueScheduler.cc ../galay-kernel/galay-kernel/kernel/IOUringScheduler.cc ../galay-kernel/test/T32-IOSchedulerLocalFirst.cc ../galay-kernel/test/T36-TaskCoreWakePath.cc
git commit -m "refactor: move scheduler ready queues to task refs"
```

### Task 5: Migrate waiter chaining and final completion to the new task core

**Files:**
- Modify: `../galay-kernel/galay-kernel/kernel/Coroutine.h`
- Modify: `../galay-kernel/galay-kernel/kernel/Coroutine.cc`
- Test: `../galay-kernel/test/T24-Spawn.cc`
- Test: `../galay-kernel/test/T25-SpawnSimple.cc`
- Test: `../galay-kernel/test/T36-TaskCoreWakePath.cc`

**Step 1: Add failing completion-order assertions first**

If `T36` does not already prove waiter ordering, add explicit assertions for:
- `co_await child.wait()` resuming the waiter once
- child completion not leaking task state

**Step 2: Run tests to verify they fail**

Run:

```bash
cmake --build ../galay-kernel/build --target T24-Spawn T25-SpawnSimple T36-TaskCoreWakePath -j4
./../galay-kernel/build/test/T24-Spawn
./../galay-kernel/build/test/T25-SpawnSimple
./../galay-kernel/build/test/T36-TaskCoreWakePath
```

Expected: at least one test fails before waiter/final completion logic is migrated.

**Step 3: Write minimal implementation**

Replace `m_next` wrapper storage with lightweight continuation linkage inside `TaskState` and make final completion release/continue correctly.

**Step 4: Run tests to verify they pass**

Run:

```bash
./../galay-kernel/build/test/T24-Spawn
./../galay-kernel/build/test/T25-SpawnSimple
./../galay-kernel/build/test/T36-TaskCoreWakePath
```

Expected: PASS.

**Step 5: Commit**

```bash
git add ../galay-kernel/galay-kernel/kernel/Coroutine.h ../galay-kernel/galay-kernel/kernel/Coroutine.cc ../galay-kernel/test/T24-Spawn.cc ../galay-kernel/test/T25-SpawnSimple.cc ../galay-kernel/test/T36-TaskCoreWakePath.cc
git commit -m "refactor: migrate waiter chaining to lightweight task core"
```

### Task 6: Re-run kernel regressions and compare scheduler benchmarks

**Files:**
- No source changes

**Step 1: Build the regression targets**

Run:

```bash
cmake --build ../galay-kernel/build --target \
  T19-ReadvWritev \
  T20-RingbufferIo \
  T24-Spawn \
  T25-SpawnSimple \
  T26-ConcurrentRecvSend \
  T31-UnsafeChannelDeferredWake \
  T32-IOSchedulerLocalFirst \
  T34-ReadvArrayBorrowed \
  T35-ReadvArrayCountGuard \
  T36-TaskCoreWakePath \
  B8-MpscChannel \
  B9-UnsafeChannel \
  B2-TcpServer \
  B3-TcpClient \
  B11-TcpIovServer \
  B12-TcpIovClient \
  -j4
```

Expected: build succeeds cleanly.

**Step 2: Run the regression tests**

Run:

```bash
./../galay-kernel/build/test/T19-ReadvWritev
./../galay-kernel/build/test/T20-RingbufferIo
./../galay-kernel/build/test/T24-Spawn
./../galay-kernel/build/test/T25-SpawnSimple
./../galay-kernel/build/test/T26-ConcurrentRecvSend
./../galay-kernel/build/test/T31-UnsafeChannelDeferredWake
./../galay-kernel/build/test/T32-IOSchedulerLocalFirst
./../galay-kernel/build/test/T34-ReadvArrayBorrowed
./../galay-kernel/build/test/T35-ReadvArrayCountGuard
./../galay-kernel/build/test/T36-TaskCoreWakePath
```

Expected: all PASS.

**Step 3: Run benchmark comparisons**

Run locally or on the remote Linux verification host:

```bash
./../galay-kernel/build/bin/B8-MpscChannel
./../galay-kernel/build/bin/B9-UnsafeChannel
```

Then run paired socket comparisons:

```bash
./../galay-kernel/build/bin/B2-TcpServer 18080
./../galay-kernel/build/bin/B3-TcpClient -h 127.0.0.1 -p 18080 -d 5
./../galay-kernel/build/bin/B11-TcpIovServer 18081
./../galay-kernel/build/bin/B12-TcpIovClient -h 127.0.0.1 -p 18081 -d 5
```

Expected:
- `B8` improves materially versus the pre-task-core baseline
- `B9` inline mode remains faster, but the gap to deferred scheduling narrows
- `B2/B3` and `B11/B12` show either improvement or no regression

**Step 4: Commit**

```bash
git add .
git commit -m "test: verify lightweight task core regressions and benchmarks"
```
