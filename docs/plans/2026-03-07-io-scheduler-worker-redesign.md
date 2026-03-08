# IO Scheduler Worker Redesign Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Redesign `epoll` and `kqueue` IO scheduler workers around local-first execution so same-thread wakeups avoid the current shared-queue dominated path.

**Architecture:** First add scheduler correctness tests that lock the desired local/LIFO/inject behavior, then implement the worker model in `KqueueScheduler`, port it to `EpollScheduler`, and finally verify the existing HTTP/2 regression and benchmark paths. Keep public awaitable and socket APIs unchanged in this phase.

**Tech Stack:** C++23 coroutines, `galay-kernel` schedulers, lock-free queues, `kqueue`, `epoll`, existing galay-http HTTP/2 regression suite.

---

### Task 1: Lock the scheduler behavior with failing kernel tests

**Files:**
- Create: `../galay-kernel/test/T32-IOSchedulerLocalFirst.cc`
- Test: `../galay-kernel/test/CMakeLists.txt`

**Step 1: Write the failing test**

Add a new kernel test file that asserts, at minimum:
- same-thread re-schedule prefers immediate local-first execution order
- repeated same-thread wakeups do not permanently starve older queued work
- cross-thread `spawn()` wakes a parked IO scheduler promptly

Use the existing scheduler selection pattern from tests like `T20-RingbufferIo.cc` / `T26-ConcurrentRecvSend.cc` so the same test builds against `KqueueScheduler` on macOS and `EpollScheduler` on Linux.

**Step 2: Run test to verify it fails**

Run:
```bash
cmake --build ../galay-kernel/build --target T32-IOSchedulerLocalFirst -j4
./../galay-kernel/build/test/T32-IOSchedulerLocalFirst
```
Expected: the test builds or runs fail before the new worker model exists.

**Step 3: Write minimal implementation support if needed for test scaffolding**

Only add the smallest helper hooks needed to express local/LIFO/inject behavior observably from the test. Do not begin the full scheduler rewrite in this step.

**Step 4: Run test to verify it still captures the missing behavior**

Run:
```bash
./../galay-kernel/build/test/T32-IOSchedulerLocalFirst
```
Expected: still FAIL until the scheduler behavior is implemented.

**Step 5: Commit**

```bash
git add ../galay-kernel/test/T32-IOSchedulerLocalFirst.cc
git commit -m "test: add IO scheduler local-first behavior guard"
```

### Task 2: Introduce shared worker-state structure for IO schedulers

**Files:**
- Modify: `../galay-kernel/galay-kernel/kernel/KqueueScheduler.h`
- Modify: `../galay-kernel/galay-kernel/kernel/EpollScheduler.h`
- Modify: `../galay-kernel/galay-kernel/kernel/Coroutine.h`
- Modify: `../galay-kernel/galay-kernel/kernel/Coroutine.cc`
- Create or Modify: `../galay-kernel/galay-kernel/kernel/IOScheduler.hpp`

**Step 1: Write the failing test usage target**

Extend `T32-IOSchedulerLocalFirst.cc` as needed so it observes:
- a fast local scheduling slot
- a local queue fallback
- an inject path distinction for cross-thread submission

**Step 2: Run test to verify it fails**

Run:
```bash
./../galay-kernel/build/test/T32-IOSchedulerLocalFirst
```
Expected: FAIL because the worker state is not yet wired in.

**Step 3: Write minimal implementation**

Introduce the scheduler-side state required for the redesign:
- optional `lifo_slot`
- worker-local queue
- inject queue / remote submission entrypoint
- fairness counters or caps

Keep the public `Scheduler` interface stable. Any helper methods added should remain scheduler-internal unless tests require narrow visibility.

**Step 4: Run test to verify partial behavior**

Run:
```bash
./../galay-kernel/build/test/T32-IOSchedulerLocalFirst
```
Expected: some assertions may still fail until `KqueueScheduler` consumes the new state.

**Step 5: Commit**

```bash
git add ../galay-kernel/galay-kernel/kernel/KqueueScheduler.h ../galay-kernel/galay-kernel/kernel/EpollScheduler.h ../galay-kernel/galay-kernel/kernel/Coroutine.h ../galay-kernel/galay-kernel/kernel/Coroutine.cc ../galay-kernel/galay-kernel/kernel/IOScheduler.hpp ../galay-kernel/test/T32-IOSchedulerLocalFirst.cc
git commit -m "refactor: add shared IO worker scheduling state"
```

### Task 3: Implement the local-first worker model in `KqueueScheduler`

**Files:**
- Modify: `../galay-kernel/galay-kernel/kernel/KqueueScheduler.h`
- Modify: `../galay-kernel/galay-kernel/kernel/KqueueScheduler.cc`
- Test: `../galay-kernel/test/T32-IOSchedulerLocalFirst.cc`

**Step 1: Write or extend the failing runtime assertions**

Make sure `T32-IOSchedulerLocalFirst.cc` specifically fails on current `kqueue` behavior for:
- same-thread wake path not taking the local-first route
- cross-thread submit not explicitly unparking the worker
- fairness cap not enforced

**Step 2: Run test to verify it fails**

Run:
```bash
./../galay-kernel/build/test/T32-IOSchedulerLocalFirst
```
Expected: FAIL on macOS before `KqueueScheduler` is updated.

**Step 3: Write minimal implementation**

Update `KqueueScheduler` to:
- distinguish same-thread scheduling from cross-thread injection
- use `lifo_slot` first for same-thread wakeups
- fall back to worker-local queue
- notify the parked worker on cross-thread injection
- cap consecutive LIFO executions and periodically check injected work before parking

Do not modify the IO registration model in this step.

**Step 4: Run tests to verify they pass**

Run:
```bash
cmake --build ../galay-kernel/build --target T32-IOSchedulerLocalFirst -j4
./../galay-kernel/build/test/T32-IOSchedulerLocalFirst
```
Expected: PASS on macOS for the `kqueue` path.

**Step 5: Commit**

```bash
git add ../galay-kernel/galay-kernel/kernel/KqueueScheduler.h ../galay-kernel/galay-kernel/kernel/KqueueScheduler.cc ../galay-kernel/test/T32-IOSchedulerLocalFirst.cc
git commit -m "feat: add local-first worker model to kqueue scheduler"
```

### Task 4: Port the same worker model to `EpollScheduler`

**Files:**
- Modify: `../galay-kernel/galay-kernel/kernel/EpollScheduler.h`
- Modify: `../galay-kernel/galay-kernel/kernel/EpollScheduler.cc`
- Test: `../galay-kernel/test/T32-IOSchedulerLocalFirst.cc`

**Step 1: Reuse the same failing test**

Confirm the new scheduler behavior guard is meaningful for the `epoll` path as well.

**Step 2: Run test to verify the Linux path still fails before porting**

Run on Linux:
```bash
cmake --build ../galay-kernel/build --target T32-IOSchedulerLocalFirst -j4
./../galay-kernel/build/test/T32-IOSchedulerLocalFirst
```
Expected: FAIL before `EpollScheduler` is updated.

**Step 3: Write minimal implementation**

Port the same model to `EpollScheduler`:
- preserve existing `eventfd` wake mechanism
- route same-thread wakeups to `lifo_slot` / local queue
- keep cross-thread submissions on inject queue + notify
- apply the same fairness rules used in `KqueueScheduler`

**Step 4: Run tests to verify it passes**

Run on Linux:
```bash
./../galay-kernel/build/test/T32-IOSchedulerLocalFirst
```
Expected: PASS.

**Step 5: Commit**

```bash
git add ../galay-kernel/galay-kernel/kernel/EpollScheduler.h ../galay-kernel/galay-kernel/kernel/EpollScheduler.cc ../galay-kernel/test/T32-IOSchedulerLocalFirst.cc
git commit -m "feat: add local-first worker model to epoll scheduler"
```

### Task 5: Re-run existing kernel regressions

**Files:**
- No source changes

**Step 1: Build the relevant kernel tests**

Run:
```bash
cmake --build ../galay-kernel/build --target T17-UnsafeChannel T20-RingbufferIo T24-Spawn T25-SpawnSimple T26-ConcurrentRecvSend T31-UnsafeChannelDeferredWake T32-IOSchedulerLocalFirst -j4
```
Expected: build succeeds.

**Step 2: Run the runtime regressions**

Run:
```bash
./../galay-kernel/build/test/T17-UnsafeChannel
./../galay-kernel/build/test/T20-RingbufferIo
./../galay-kernel/build/test/T24-Spawn
./../galay-kernel/build/test/T25-SpawnSimple
./../galay-kernel/build/test/T26-ConcurrentRecvSend
./../galay-kernel/build/test/T31-UnsafeChannelDeferredWake
./../galay-kernel/build/test/T32-IOSchedulerLocalFirst
```
Expected: PASS.

**Step 3: Commit**

```bash
git add ../galay-kernel
git commit -m "test: verify IO scheduler worker redesign regressions"
```

### Task 6: Re-verify galay-http HTTP/2 regressions against the new kernel

**Files:**
- No source changes in `galay-http` expected for this task

**Step 1: Build the HTTP/2 regression targets**

Run:
```bash
cmake --build build-test --target T41-H2CloseTcpTeardown T42-H2ActiveConnApi T43-H2ActiveConnPreferred T44-H2cClientShutdown T45-H2AwaitableSurface T46-H2ActiveConnRetire T47-IoVecBorrowCursor -j4
```
Expected: build succeeds.

**Step 2: Run the HTTP/2 regressions**

Run:
```bash
./build-test/test/T41-H2CloseTcpTeardown
./build-test/test/T42-H2ActiveConnApi
./build-test/test/T43-H2ActiveConnPreferred
./build-test/test/T44-H2cClientShutdown
./build-test/test/T45-H2AwaitableSurface
./build-test/test/T46-H2ActiveConnRetire
./build-test/test/T47-IoVecBorrowCursor
```
Expected: PASS.

**Step 3: Run real H2c client/server validation**

Run:
```bash
cmake --build build-ssl-nolog --target B3-H2cServer -j4
./build-ssl-nolog/benchmark/B3-H2cServer 29080 1 0 &
./build-test/test/T25-H2cClient 127.0.0.1 29080 3
```
Expected: `Success: 3`, `Failed: 0`.

**Step 4: Commit**

```bash
git add build-test build-ssl-nolog
# If no source files changed in galay-http for this task, skip commit.
```

### Task 7: Re-run h2c benchmark and compare against prior baselines

**Files:**
- Create: `benchmark/results/<timestamp>-h2c-after-io-scheduler-worker-redesign/`

**Step 1: Build the benchmark server**

Run:
```bash
cmake --build build-ssl-nolog --target B3-H2cServer -j4
```
Expected: build succeeds.

**Step 2: Run the same h2c comparison used for the current baseline**

Run the existing Go benchmark client against:
- current `galay` h2c server
- Go reference h2c server
- Rust/Tokio reference h2c server

Prefer matching the current `140 conns / 8s / 128B / /echo / 4 workers` setup.

**Step 3: Save outputs**

Store results under:
```text
benchmark/results/<timestamp>-h2c-after-io-scheduler-worker-redesign/
```
Include:
- `aggregate.csv`
- per-run logs
- optional profile samples if the new result is ambiguous

**Step 4: Compare against existing baselines**

Compare to:
- `benchmark/results/20260307-095056-h2c-after-retire-fix/aggregate.csv`
- `benchmark/results/20260307-111357-h2c-current-one-shot/aggregate.csv`

**Step 5: Commit**

```bash
git add benchmark/results/<timestamp>-h2c-after-io-scheduler-worker-redesign
git commit -m "bench: record h2c results after IO scheduler worker redesign"
```
