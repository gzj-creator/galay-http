# IOUring Worker Model Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Move `IOUringScheduler` onto the same local-first worker model already used by `kqueue` and `epoll`.

**Architecture:** Reuse `IOSchedulerWorkerState` and the `spawn()` / `spawnDeferred()` split already introduced in the scheduler base path. Keep all existing SQE/CQE submission and completion handling unchanged in this batch.

**Tech Stack:** C++23 coroutines, `io_uring`, `eventfd`, `IOSchedulerWorkerState`, existing galay-kernel regression tests.

---

### Task 1: Extend the scheduler behavior guard to `io_uring`

**Files:**
- Modify: `../galay-kernel/test/T32-IOSchedulerLocalFirst.cc`

**Step 1: Write the failing test**

Add an `#elif defined(USE_IOURING)` branch so `T32-IOSchedulerLocalFirst` can bind `IOUringScheduler`.

**Step 2: Run test to verify it fails**

Run on Linux with `GALAY_KERNEL_BACKEND=io_uring`:
```bash
cmake -S ../galay-kernel -B ../galay-kernel/build-io-uring -DGALAY_KERNEL_BACKEND=io_uring
cmake --build ../galay-kernel/build-io-uring --target T32-IOSchedulerLocalFirst -j4
./../galay-kernel/build-io-uring/bin/T32-IOSchedulerLocalFirst
```
Expected: FAIL before `IOUringScheduler` is updated.

**Step 3: Commit**

```bash
git add ../galay-kernel/test/T32-IOSchedulerLocalFirst.cc
git commit -m "test: extend IO scheduler local-first guard to io_uring"
```

### Task 2: Port the worker model to `IOUringScheduler`

**Files:**
- Modify: `../galay-kernel/galay-kernel/kernel/IOUringScheduler.h`
- Modify: `../galay-kernel/galay-kernel/kernel/IOUringScheduler.cc`
- Reference: `../galay-kernel/galay-kernel/kernel/KqueueScheduler.cc`
- Reference: `../galay-kernel/galay-kernel/kernel/EpollScheduler.cc`

**Step 1: Write minimal implementation**

Update `IOUringScheduler` so that:
- `spawn()` uses local-first for same-thread scheduling
- `spawnDeferred()` enqueues into local queue without preempting already-ready work
- cross-thread `spawn()` and `spawnDeferred()` use inject queue + `notify()`
- `processPendingCoroutines()` drains from `IOSchedulerWorkerState`
- old coroutine queue fields are removed from the scheduler state

**Step 2: Run the focused test**

Run on Linux:
```bash
cmake --build ../galay-kernel/build-io-uring --target T32-IOSchedulerLocalFirst -j4
./../galay-kernel/build-io-uring/bin/T32-IOSchedulerLocalFirst
```
Expected: PASS.

**Step 3: Commit**

```bash
git add ../galay-kernel/galay-kernel/kernel/IOUringScheduler.h ../galay-kernel/galay-kernel/kernel/IOUringScheduler.cc
git commit -m "feat: add local-first worker model to io_uring scheduler"
```

### Task 3: Re-run key regressions under `io_uring`

**Files:**
- Test only

**Step 1: Build the regression set**

```bash
cmake --build ../galay-kernel/build-io-uring \
  --target T17-UnsafeChannel T24-Spawn T25-SpawnSimple T26-ConcurrentRecvSend T31-UnsafeChannelDeferredWake T32-IOSchedulerLocalFirst \
  -j4
```

**Step 2: Run the regression set**

```bash
./../galay-kernel/build-io-uring/bin/T17-UnsafeChannel
./../galay-kernel/build-io-uring/bin/T24-Spawn
./../galay-kernel/build-io-uring/bin/T25-SpawnSimple
./../galay-kernel/build-io-uring/bin/T26-ConcurrentRecvSend
./../galay-kernel/build-io-uring/bin/T31-UnsafeChannelDeferredWake
./../galay-kernel/build-io-uring/bin/T32-IOSchedulerLocalFirst
```
Expected: all PASS.

**Step 3: Record platform limitation if not executable locally**

If working on macOS, document that only static symmetry checks were done locally and Linux verification remains required.

**Step 4: Commit**

```bash
git add ../galay-kernel/test/T32-IOSchedulerLocalFirst.cc
git commit -m "test: verify io_uring worker model regressions"
```
