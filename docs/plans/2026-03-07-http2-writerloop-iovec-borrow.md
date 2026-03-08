# HTTP/2 WriterLoop IoVec Borrow Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Remove extra descriptor churn from the HTTP/2 `writerLoop` hot path by using a borrowed `iovec` cursor over the local `iovecs` buffer.

**Architecture:** Add a borrowed cursor utility in `IoVecUtils.h` that mutates caller-owned `iovec` storage in place, then switch `Http2StreamManager::writerLoop` to use that cursor and pass the current window directly to `socket.writev(...)`. Keep public `writev` awaitable semantics unchanged.

**Tech Stack:** C++23, `std::span`, existing `IoVecCursor` utilities, HTTP/2 stream manager tests, h2c benchmark.

---

### Task 1: Lock borrowed cursor behavior with a failing test

**Files:**
- Create: `test/T47-IoVecBorrowCursor.cc`
- Modify: `galay-http/kernel/IoVecUtils.h`

**Step 1: Write the failing test**

Add a minimal test that requires:
- `BorrowedIoVecCursor` construction from mutable `iovec` storage
- `count()` and `data()` to expose the current window
- `advance()` to mutate the original `iovec` descriptors in place

**Step 2: Run test to verify it fails**

Run: `cmake -S . -B build-test >/dev/null && cmake --build build-test --target T47-IoVecBorrowCursor -j4`
Expected: compile failure because `BorrowedIoVecCursor` does not exist yet.

**Step 3: Write minimal implementation**

Add a borrowed cursor with:
- constructor/reset from `std::span<struct iovec>`
- `data()`, `count()`, `empty()`, `advance()`
- internal normalization that compacts zero-length segments in place

**Step 4: Run test to verify it passes**

Run: `./build-test/test/T47-IoVecBorrowCursor`
Expected: PASS.

### Task 2: Switch HTTP/2 writerLoop to borrowed cursor

**Files:**
- Modify: `galay-http/kernel/http2/Http2StreamManager.h`
- Test: `test/T43-H2ActiveConnPreferred.cc`
- Test: `test/T44-H2cClientShutdown.cc`
- Test: `test/T46-H2ActiveConnRetire.cc`

**Step 1: Write/extend the failing guard if needed**

If the borrowed cursor test already locks the utility behavior, do not add a second synthetic test. Reuse the existing HTTP/2 regression suite as the behavior guard.

**Step 2: Run the targeted regressions before the change**

Run:
- `cmake --build build-test --target T43-H2ActiveConnPreferred T44-H2cClientShutdown T46-H2ActiveConnRetire -j4`
- `./build-test/test/T43-H2ActiveConnPreferred`
- `./build-test/test/T44-H2cClientShutdown`
- `./build-test/test/T46-H2ActiveConnRetire`

Expected: PASS baseline before refactor.

**Step 3: Write minimal implementation**

Change `writerLoop` to:
- build `iovecs` once
- create a `BorrowedIoVecCursor` over `iovecs`
- call `m_conn.socket().writev(std::span<const iovec>(cursor.data(), cursor.count()))`
- remove `submit_iovecs` and `exportWindow()`

**Step 4: Run regressions to verify they still pass**

Run:
- `./build-test/test/T43-H2ActiveConnPreferred`
- `./build-test/test/T44-H2cClientShutdown`
- `./build-test/test/T46-H2ActiveConnRetire`

Expected: PASS.

### Task 3: Verify benchmark build and h2c performance

**Files:**
- No additional source files

**Step 1: Rebuild benchmark server**

Run: `cmake --build build-ssl-nolog --target B3-H2cServer -j4`
Expected: build succeeds.

**Step 2: Re-run h2c benchmark**

Run the same `140 conns / 8s / 128B / /echo / 4 workers` benchmark against the current inline baseline.

**Step 3: Record results**

Save aggregate output under `benchmark/results/<timestamp>-h2c-writerloop-iovec-borrow/`.

**Step 4: Compare against prior baseline**

Compare to:
- `benchmark/results/20260307-095056-h2c-after-retire-fix/aggregate.csv`
- `benchmark/results/20260307-103154-h2c-inline-vs-deferred-ab/ab_runs.csv`
