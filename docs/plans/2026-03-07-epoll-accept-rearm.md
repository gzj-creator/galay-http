# Epoll Accept Rearm Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Fix repeated `co_await accept()` on the same listener under Linux `EpollScheduler` so the TCP benchmarks become trustworthy again.

**Architecture:** Keep the change inside `EpollScheduler::addAccept()`. Match the existing `recv/send` epoll registration pattern by attempting `EPOLL_CTL_MOD` first and falling back to `EPOLL_CTL_ADD` on `ENOENT`. Guard the behavior with a Linux-only regression test that repeatedly accepts on one listener.

**Tech Stack:** C++20 coroutines, `epoll`, existing `galay-kernel` test harness, benchmark binaries.

---

### Task 1: Add a Linux-only failing regression test

**Files:**
- Create: `../galay-kernel/test/T33-EpollAcceptRearm.cc`
- Modify: `../galay-kernel/test/CMakeLists.txt`

**Step 1: Write the failing test**

Create a minimal runtime regression that:

- starts `EpollScheduler`
- binds a loopback listener on an ephemeral port
- spawns an accept coroutine that awaits `accept()` twice on the same listener
- connects two clients sequentially
- records the second accept error if it occurs
- expects both accepts to succeed

The assertion must specifically fail if the second accept hits `EEXIST` or any other unexpected error.

**Step 2: Run test to verify it fails**

Run:
```bash
cmake --build build --target T33-EpollAcceptRearm -j4
./build/test/T33-EpollAcceptRearm
```

Expected: FAIL on Linux before the scheduler fix, ideally with an error showing `File exists` or a missing second accepted connection.

**Step 3: Commit**

```bash
git add test/T33-EpollAcceptRearm.cc test/CMakeLists.txt
git commit -m "test: add epoll accept rearm regression"
```

### Task 2: Implement the minimal epoll fix

**Files:**
- Modify: `../galay-kernel/galay-kernel/kernel/EpollScheduler.cc`
- Test: `../galay-kernel/test/T33-EpollAcceptRearm.cc`

**Step 1: Write minimal implementation**

Update `EpollScheduler::addAccept()` so that when synchronous completion does not finish immediately:

- build the epoll event as today
- call `epoll_ctl(..., EPOLL_CTL_MOD, ...)`
- if that fails with `ENOENT`, retry with `EPOLL_CTL_ADD`

Do not change `IOController`, awaitable APIs, or benchmark logic in this task.

**Step 2: Run the regression test to verify it passes**

Run:
```bash
cmake --build build --target T33-EpollAcceptRearm -j4
./build/test/T33-EpollAcceptRearm
```

Expected: PASS on Linux.

**Step 3: Commit**

```bash
git add galay-kernel/kernel/EpollScheduler.cc test/T33-EpollAcceptRearm.cc
git commit -m "fix: rearm epoll accept registrations"
```

### Task 3: Smoke-check existing TCP paths

**Files:**
- No source changes

**Step 1: Build the affected runtime tests and benchmarks**

Run:
```bash
cmake --build build --target T26-ConcurrentRecvSend B2-TcpServer B3-TcpClient B11-TcpIovServer B12-TcpIovClient -j4
```

Expected: build succeeds.

**Step 2: Run runtime smoke verification**

Run:
```bash
./build/test/T26-ConcurrentRecvSend
```

Expected: PASS.

**Step 3: Run benchmark smoke verification on Linux**

Run server and client in separate shells:
```bash
./build/benchmark/B2-TcpServer 18080
./build/benchmark/B3-TcpClient -h 127.0.0.1 -p 18080 -c 20 -s 256 -d 3
```

Then:
```bash
./build/benchmark/B11-TcpIovServer 18081
./build/benchmark/B12-TcpIovClient -h 127.0.0.1 -p 18081 -c 20 -s 256 -d 3
```

Expected:

- servers accept connections without `File exists`
- clients report non-zero requests
- no obvious listener stall

**Step 4: Commit**

```bash
git commit --allow-empty -m "chore: verify epoll tcp benchmark path"
```
