# Readv/Writev Array Borrowed Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add same-name array overloads for borrowed `readv/writev` hot paths while preserving the existing safe `span` overloads.

**Architecture:** Keep one `ReadvAwaitable` and one `WritevAwaitable`, but make their IO contexts dual-mode: either own copied iovecs for the current safe path or borrow array-backed iovecs through a `std::span<const iovec>`. Add array convenience helpers to `RingBuffer` and switch `B11/B12` to the borrowed array path.

**Tech Stack:** C++20, coroutines, `std::span`, `std::array`, `epoll/kqueue/io_uring` shared awaitable path, existing kernel benchmarks/tests.

---

### Task 1: Add a failing borrowed-array regression test

**Files:**
- Create: `../galay-kernel/test/T34-ReadvArrayBorrowed.cc`

**Step 1: Write the failing test**

Create a runtime test that:

- starts the current scheduler backend
- creates a loopback TCP client/server pair
- uses `std::array<iovec, 2>` with `socket.readv(iovecs, count)` and `socket.writev(iovecs, count)`
- validates the full request/response payload

Before implementation, this test should fail to build because the array overloads do not exist.

**Step 2: Run test to verify it fails**

Run:
```bash
cmake -S . -B build
cmake --build build --target T34-ReadvArrayBorrowed -j4
```

Expected: build failure with no matching `readv/writev` overload for `std::array<iovec, N>`.

**Step 3: Commit**

```bash
git add test/T34-ReadvArrayBorrowed.cc
git commit -m "test: add readv array borrowed regression"
```

### Task 2: Implement dual-mode readv/writev awaitable storage

**Files:**
- Modify: `../galay-kernel/galay-kernel/kernel/Awaitable.h`
- Modify: `../galay-kernel/galay-kernel/kernel/Awaitable.inl`

**Step 1: Write minimal implementation**

Update `ReadvIOContext` and `WritevIOContext` so they hold:

- `std::vector<iovec> m_owned_iovecs`
- `std::span<const iovec> m_iovecs`

Add constructors for:

- safe `std::span<const iovec>` copy path
- borrowed `std::array<iovec, N>&`
- borrowed `iovec (&)[N]`

Keep `handleComplete()` using `m_iovecs.data()` and `m_iovecs.size()`.

**Step 2: Run the borrowed-array test**

Run:
```bash
cmake --build build --target T34-ReadvArrayBorrowed -j4
./build/bin/T34-ReadvArrayBorrowed
```

Expected: test may still fail until `TcpSocket` exposes the overloads.

**Step 3: Commit**

```bash
git add galay-kernel/kernel/Awaitable.h galay-kernel/kernel/Awaitable.inl test/T34-ReadvArrayBorrowed.cc
git commit -m "refactor: support borrowed iovec storage in readv awaitables"
```

### Task 3: Add public array overloads and RingBuffer helpers

**Files:**
- Modify: `../galay-kernel/galay-kernel/async/TcpSocket.h`
- Modify: `../galay-kernel/galay-kernel/async/TcpSocket.cc`
- Modify: `../galay-kernel/galay-kernel/common/Buffer.h`

**Step 1: Add the overloads**

Expose:

- `template<size_t N> readv(std::array<iovec, N>&, size_t count = N)`
- `template<size_t N> writev(std::array<iovec, N>&, size_t count = N)`
- `template<size_t N> readv(iovec (&)[N], size_t count = N)`
- `template<size_t N> writev(iovec (&)[N], size_t count = N)`

Forward them into the new borrowed constructors.

Add array-based `RingBuffer::getReadIovecs()` / `getWriteIovecs()` helpers that forward to the existing pointer-based implementation.

**Step 2: Re-run the borrowed-array test**

Run:
```bash
cmake --build build --target T34-ReadvArrayBorrowed -j4
./build/bin/T34-ReadvArrayBorrowed
```

Expected: PASS.

**Step 3: Commit**

```bash
git add galay-kernel/async/TcpSocket.h galay-kernel/async/TcpSocket.cc galay-kernel/common/Buffer.h test/T34-ReadvArrayBorrowed.cc
git commit -m "feat: add array overloads for borrowed readv and writev"
```

### Task 4: Switch benchmark hot paths to the borrowed array overloads

**Files:**
- Modify: `../galay-kernel/benchmark/B11-TcpIovServer.cc`
- Modify: `../galay-kernel/benchmark/B12-TcpIovClient.cc`

**Step 1: Update benchmark call sites**

Replace vector-building iovec calls with:

- stack `std::array<iovec, 2>`
- `RingBuffer` array helper
- `socket.readv(iovecs, count)` / `socket.writev(iovecs, count)`

Do not change benchmark control flow or socket logic in this task.

**Step 2: Run benchmark builds**

Run:
```bash
cmake --build build --target B11-TcpIovServer B12-TcpIovClient -j4
```

Expected: build succeeds.

**Step 3: Commit**

```bash
git add benchmark/B11-TcpIovServer.cc benchmark/B12-TcpIovClient.cc
git commit -m "perf: use array-based readv and writev hot path"
```

### Task 5: Regression and benchmark verification

**Files:**
- No source changes

**Step 1: Run local behavior regressions**

Run:
```bash
cmake --build build --target T19-ReadvWritev T20-RingbufferIo T34-ReadvArrayBorrowed -j4
./build/bin/T19-ReadvWritev
./build/bin/T20-RingbufferIo
./build/bin/T34-ReadvArrayBorrowed
```

Expected: all pass.

**Step 2: Run remote Linux benchmark comparison**

Run on the verification host:
```bash
cmake -S . -B build -DGALAY_KERNEL_BACKEND=epoll -DCMAKE_BUILD_TYPE=Release
cmake --build build --target B2-TcpServer B3-TcpClient B11-TcpIovServer B12-TcpIovClient -j4
```

Then run smoke/perf checks:
```bash
./build/bin/B2-TcpServer 18080
./build/bin/B3-TcpClient -h 127.0.0.1 -p 18080 -c 20 -s 256 -d 3
./build/bin/B11-TcpIovServer 18081
./build/bin/B12-TcpIovClient -h 127.0.0.1 -p 18081 -c 20 -s 256 -d 3
```

Expected:

- no behavior regressions
- `B11/B12` materially improve over the previous `~67.8k QPS` baseline
- `B2/B3` remain healthy

**Step 3: Commit**

```bash
git commit --allow-empty -m "chore: verify array-based readv benchmark improvement"
```
