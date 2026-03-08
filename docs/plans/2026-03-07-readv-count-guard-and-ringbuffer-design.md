# Readv Count Guard And RingBuffer Hot Path Design

**Date:** 2026-03-07

**Goal:** First turn array-based borrowed `readv/writev` count overflow into a hard failure, then extend the array hot path to the remaining runtime-heavy `RingBuffer` call sites without hurting readability.

**Scope:** This design covers the same-name array overloads for `TcpSocket::readv/writev`, their borrowed count validation, `RingBuffer` array helpers, and the remaining benchmark/runtime hot paths that still build transient `std::vector<iovec>`.

## Background

The previous batch added same-name array overloads for borrowed `readv/writev` and switched `B11/B12` to the array hot path. That removed a large chunk of iovec metadata overhead and improved remote Linux `B11/B12` throughput materially.

Two follow-up items remain:

1. The current array borrowed path silently clamps `count` to the array capacity.
2. Some runtime-heavy `RingBuffer` paths still use the vector-building helpers even though array helpers now exist.

The user explicitly wants these handled in order: first the count guard, then the remaining hot path convergence.

## Problem Statement

### 1. Silent borrowed count clamping is unsafe

Today, calling:

```cpp
std::array<iovec, 2> iovecs{};
socket.readv(iovecs, 3);
```

does not fail loudly. It silently truncates the count to `2`.

That is bad for two reasons:

- it hides a caller bug in the “high-performance but stricter” path
- it can make benchmark and runtime behavior appear correct while operating on a different iovec set than the caller intended

### 2. Some `RingBuffer` hot paths still pay vector construction cost

Although `B11/B12` already moved to arrays, other loops still use `getReadIovecs()/getWriteIovecs()` returning `std::vector<iovec>`. That keeps avoidable dynamic allocation in places where the segment count is still small and predictable.

## Approaches Considered

### A. Keep clamping, just document it

Pros:

- no behavior change
- smallest code delta

Cons:

- preserves a silent correctness hazard
- conflicts with the user's request to tighten behavior

### B. Debug-only assert, release still clamp

Pros:

- developer-friendly during debugging
- no hard abort in production

Cons:

- release benchmark and runtime builds would still hide mistakes
- inconsistent semantics across build modes

### C. Always hard-fail on `count > N`, then converge runtime hot paths

Pros:

- makes invalid borrowed usage unmissable
- keeps the fast path strict and easy to reason about
- matches the user's requested ordering

Cons:

- invalid callers now terminate instead of continuing

## Chosen Approach

Choose **C**.

Borrowed array overloads are explicitly the strict path. They should not silently reinterpret caller intent. If the caller claims `count > N`, the program should fail immediately in every build mode.

For the second step, only change the real runtime-heavy paths first. Keep the vector-returning `RingBuffer` helpers for readability and for tests that specifically verify vector semantics.

## Design

### Count Guard

Replace the current normalization/clamping helper with a tiny always-on validation helper:

- input: `count`, `capacity`, and a short operation label (`"readv"` / `"writev"`)
- behavior:
  - if `count <= capacity`, return `count`
  - otherwise print a short diagnostic to `stderr` and `std::abort()`

This helper should only be used by the borrowed array constructors. The existing safe `span` overloads keep their current behavior because they copy exactly what the caller provides.

### RingBuffer Hot Path Convergence

After the guard lands, switch the remaining runtime-heavy sites to arrays where it improves actual execution paths:

- `benchmark/B10-Ringbuffer.cc`
- `test/T20-RingbufferIo.cc` runtime `co_await` sections

Do not rewrite every structural vector test to arrays. Where a test is asserting the shape of `getReadIovecs()/getWriteIovecs()` return values, keeping the vector-based helper is fine and clearer.

### Readability Rules

- keep the existing vector-returning `RingBuffer` helpers
- prefer array helpers only in hot loops or borrowed `readv/writev` call sites
- avoid introducing new public borrowed types
- keep the public API surface stable apart from the already approved array overloads

## Testing Strategy

1. Add a focused count-guard test that verifies `count > N` aborts
2. Confirm it fails before implementation because the current code exits normally
3. Implement the hard guard
4. Re-run existing `T34` borrowed-path behavior test
5. Update the remaining hot paths
6. Re-run `T19`, `T20`, `T34`, and remote Linux benchmark smoke/perf

## Success Criteria

- `count > N` on the borrowed array overload aborts in every build mode
- valid borrowed array calls still pass existing behavior tests
- the remaining runtime-heavy `RingBuffer` loops stop constructing transient iovec vectors
- readability stays acceptable because vector helpers remain available where they are not hot
