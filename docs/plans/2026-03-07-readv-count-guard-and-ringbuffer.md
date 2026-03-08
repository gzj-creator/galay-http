# Readv Count Guard And RingBuffer Hot Path Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make borrowed array `readv/writev` reject `count > N` with a hard failure, then convert the remaining runtime-heavy `RingBuffer` paths to array helpers.

**Architecture:** Keep the existing same-name array overload design. Replace borrowed count clamping with an always-on validation helper that aborts on invalid counts. After that behavior is locked, update only the hot loops that still build temporary `std::vector<iovec>`, leaving structural vector helpers and non-hot assertions intact.

**Tech Stack:** C++20, coroutines, `std::array`, `std::abort`, existing kernel tests/benchmarks, remote Linux epoll verification.

---

### Task 1: Add a failing borrowed-count guard test

**Files:**
- Create: `../galay-kernel/test/T35-ReadvArrayCountGuard.cc`

**Step 1: Write the failing test**

Create a test that forks a child process and triggers:

- `socket.readv(iovecs, N + 1)`
- `socket.writev(iovecs, N + 1)`

The parent should assert that both children terminate via `SIGABRT`.

**Step 2: Run test to verify it fails**

Run:
```bash
cmake --build build --target T35-ReadvArrayCountGuard -j4
./build/bin/T35-ReadvArrayCountGuard
```

Expected: FAIL because the child exits normally under the current silent clamp behavior.

**Step 3: Commit**

```bash
git add test/T35-ReadvArrayCountGuard.cc
git commit -m "test: add borrowed readv count guard regression"
```

### Task 2: Implement the hard borrowed-count guard

**Files:**
- Modify: `../galay-kernel/galay-kernel/kernel/Awaitable.h`

**Step 1: Write minimal implementation**

Replace the current borrowed count normalization helper with an always-on validator that:

- returns `count` when valid
- prints a short message to `stderr`
- calls `std::abort()` when `count > capacity`

Use it only in borrowed array constructors.

**Step 2: Re-run the guard test**

Run:
```bash
cmake --build build --target T35-ReadvArrayCountGuard T34-ReadvArrayBorrowed -j4
./build/bin/T35-ReadvArrayCountGuard
./build/bin/T34-ReadvArrayBorrowed
```

Expected: both PASS.

**Step 3: Commit**

```bash
git add galay-kernel/kernel/Awaitable.h test/T35-ReadvArrayCountGuard.cc
git commit -m "fix: abort on invalid borrowed readv count"
```

### Task 3: Converge remaining RingBuffer runtime hot paths

**Files:**
- Modify: `../galay-kernel/benchmark/B10-Ringbuffer.cc`
- Modify: `../galay-kernel/test/T20-RingbufferIo.cc`

**Step 1: Update only runtime-heavy loops**

Change the hot loops that currently do:

- `auto iovecs = buffer.getReadIovecs();`
- `auto iovecs = buffer.getWriteIovecs();`

into array helper usage where the data flows directly into `readv/writev` or throughput-critical loops.

Do not rewrite structural vector-shape assertions unless needed.

**Step 2: Re-run local regressions**

Run:
```bash
cmake --build build --target T19-ReadvWritev T20-RingbufferIo T34-ReadvArrayBorrowed T35-ReadvArrayCountGuard B10-Ringbuffer -j4
./build/bin/T19-ReadvWritev
./build/bin/T20-RingbufferIo
./build/bin/T34-ReadvArrayBorrowed
./build/bin/T35-ReadvArrayCountGuard
```

Expected: all PASS.

**Step 3: Commit**

```bash
git add benchmark/B10-Ringbuffer.cc test/T20-RingbufferIo.cc
git commit -m "perf: use array helpers in ringbuffer hot paths"
```

### Task 4: Remote verification

**Files:**
- No source changes

**Step 1: Run remote Linux build and tests**

Run:
```bash
cmake -S . -B build -DGALAY_KERNEL_BACKEND=epoll -DCMAKE_BUILD_TYPE=Release
cmake --build build --target T19-ReadvWritev T20-RingbufferIo T34-ReadvArrayBorrowed T35-ReadvArrayCountGuard B10-Ringbuffer B11-TcpIovServer B12-TcpIovClient -j4
```

Then:
```bash
./build/bin/T19-ReadvWritev
./build/bin/T20-RingbufferIo
./build/bin/T34-ReadvArrayBorrowed
./build/bin/T35-ReadvArrayCountGuard
```

Expected: all PASS.

**Step 2: Run remote benchmark smoke/perf**

Run:
```bash
./build/bin/B11-TcpIovServer 18081
./build/bin/B12-TcpIovClient -h 127.0.0.1 -p 18081 -c 20 -s 256 -d 3
```

Optionally compare with:
```bash
./build/bin/B10-Ringbuffer
```

Expected:

- no regressions from the hard guard
- `B11/B12` remains at least as good as the previous array-hot-path result

**Step 3: Commit**

```bash
git commit --allow-empty -m "chore: verify readv count guard and ringbuffer hot paths"
```
