# Readv/Writev Array Borrowed Design

**Date:** 2026-03-07

**Goal:** Reduce small-packet `readv/writev` fixed overhead in `galay-kernel` while keeping the public API readable and preserving the current safe `span`-based behavior.

**Scope:** This design covers `TcpSocket::readv/writev`, `ReadvAwaitable/WritevAwaitable`, `RingBuffer` convenience helpers, and the `B11/B12` TCP iovec benchmarks. It does not change scheduler behavior, `epoll/kqueue/io_uring` registration semantics, or unrelated HTTP/2 code.

## Background

After fixing the Linux `accept` re-arm bug, the TCP benchmarks became trustworthy again. Fresh remote smoke results showed:

- `B2/B3` plain `send/recv`: about `79k QPS`
- `B11/B12` `readv/writev`: about `67.8k QPS`

That is the opposite of what a useful scatter-gather fast path should deliver for this workload. Code inspection shows the current `readv/writev` path pays two layers of iovec management overhead:

1. `RingBuffer::getReadIovecs()/getWriteIovecs()` often builds a `std::vector<iovec>`
2. `ReadvAwaitable/WritevAwaitable` then copies the incoming `span` into another `std::vector<iovec>`

For the common hot path where the ring buffer exposes at most 2 segments, this extra work dominates the syscall benefit.

## Problem Statement

The current `readv/writev` API is safe but too expensive for hot small-packet paths because it always copies iovec metadata into owned storage.

At the same time, simply changing the existing `readv/writev(std::span<const iovec>)` overloads to borrow externally owned spans would silently weaken safety:

- lvalue `vector` arguments would remain okay
- temporary containers would become dangling after coroutine suspension

The solution must reduce iovec overhead without hiding a new lifetime trap behind an unchanged API.

## Approaches Considered

### A. Replace current `span` overloads with borrowed semantics

Pros:

- smallest code change
- lowest metadata overhead

Cons:

- silently changes safety semantics of existing API
- temporary containers could dangle after suspension
- poor readability because borrowed behavior is invisible

### B. Add explicit `readvBorrowed/writevBorrowed`

Pros:

- semantics are explicit
- easy to document and reason about

Cons:

- call sites become noisier
- user asked to prefer same-name overloads if possible

### C. Keep safe `span` overloads and add same-name array overloads for borrowed semantics

Pros:

- readable call sites: `socket.readv(iovecs, count)`
- preserves existing safe `span` behavior unchanged
- borrowed semantics only activate for array lvalues
- avoids temporary `std::array` misuse if the overloads require non-const lvalue references

Cons:

- overload set is more nuanced
- lifetime rule still exists, but is narrower and more visible

## Chosen Approach

Choose **C. Keep safe `span` overloads and add same-name array overloads for borrowed semantics**.

This gives the hot path a clear zero-copy metadata route while keeping the existing general-purpose API intact. The key safeguard is that borrowed overloads accept only array lvalues:

- `std::array<iovec, N>&`
- `iovec (&)[N]`

That blocks the most dangerous misuse category: passing temporary arrays into a coroutine-suspending borrowed awaitable.

## Design

### Public API Surface

Keep existing safe overloads:

- `ReadvAwaitable readv(std::span<const iovec> iovecs);`
- `WritevAwaitable writev(std::span<const iovec> iovecs);`

Add borrowed same-name overloads:

- `template<size_t N> ReadvAwaitable readv(std::array<iovec, N>& iovecs, size_t count = N);`
- `template<size_t N> WritevAwaitable writev(std::array<iovec, N>& iovecs, size_t count = N);`
- `template<size_t N> ReadvAwaitable readv(iovec (&iovecs)[N], size_t count = N);`
- `template<size_t N> WritevAwaitable writev(iovec (&iovecs)[N], size_t count = N);`

Borrowed overloads intentionally return the same awaitable type. The behavior difference is represented by the constructor path, not by a separate public type.

### Awaitable Storage Model

`ReadvIOContext` and `WritevIOContext` will become dual-mode:

- safe mode:
  - own a copied `std::vector<iovec>`
  - expose a `std::span<const iovec>` pointing at the owned vector
- borrowed mode:
  - no owned vector copy
  - `std::span<const iovec>` points directly at the caller-owned array

`handleComplete()` and scheduler code should only consume the unified `span`, so scheduler dispatch does not need a parallel borrowed awaitable hierarchy.

### RingBuffer Helpers

Keep current methods:

- `std::vector<iovec> getReadIovecs() const`
- `std::vector<iovec> getWriteIovecs()`
- pointer-based `getReadIovecs(out, max)` / `getWriteIovecs(out, max)`

Add array convenience helpers:

- `template<size_t N> size_t getReadIovecs(std::array<iovec, N>& out) const`
- `template<size_t N> size_t getWriteIovecs(std::array<iovec, N>& out) const`

These helpers simply forward to the pointer-based implementation. They exist to keep hot-path call sites short and readable.

### Benchmark Hot Path

`B11/B12` should switch from vector-building usage to array borrowed usage:

```cpp
std::array<struct iovec, 2> iovecs{};
size_t count = buffer.getReadIovecs(iovecs);
auto result = co_await socket.writev(iovecs, count);
```

This removes:

- vector construction in the benchmark call site
- vector copy inside the awaitable

### Lifetime Rules

Borrowed overloads require the array to remain alive until `co_await` completes.

Safe:

- local `std::array<iovec, N>` in the coroutine frame
- local C array `iovec raw[N]`

Blocked at compile time:

- temporary `std::array<iovec, N>{...}` passed directly into borrowed overloads

Still safe through the old overload:

- `std::vector<iovec>` and generic spans continue to use the copying path

## Testing Strategy

1. Add a new test covering borrowed array overloads for `readv/writev`
2. Verify it fails before implementation because overloads do not exist yet
3. Implement the dual-mode awaitable storage and array overloads
4. Re-run the borrowed test and existing `T19/T20` behavior tests
5. Re-run remote `B11/B12` and compare against the previous `67.8k QPS` baseline

## Success Criteria

- current `span` overload behavior remains unchanged
- array overloads compile and work correctly across suspend/resume
- `B11/B12` small-packet throughput improves materially over the current baseline
- code remains readable without introducing a parallel borrowed awaitable type tree
