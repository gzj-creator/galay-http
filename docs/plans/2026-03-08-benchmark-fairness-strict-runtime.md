# Benchmark Fairness Strict Runtime Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make Galay benchmark runs use exact scheduler counts when requested, so benchmark binaries can truly run with `4 IO + 0 compute`, then re-baseline `ws`, `wss`, `h2c`, and `h2` under that stricter fairness policy.

**Architecture:** Add an opt-in strict scheduler-count mode to `galay-kernel::Runtime`, thread it through the HTTP/HTTPS/H2/H2c server builders, and enable it explicitly in benchmark binaries. Use benchmark stdout to print both configured and actual runtime scheduler counts after startup so future results are self-describing.

**Tech Stack:** `galay-kernel` runtime config/builders, `galay-http` server builders and benchmark binaries, C++23 tests, existing Go compare client/server and Rust compare server.

---

### Task 1: Lock the runtime contract with tests

**Files:**
- Add: `../galay-kernel/test/T42-runtime_strict_scheduler_counts.cc`
- Possibly add: a small compile-surface test in `test/` if builder API needs locking

**Step 1: Write a failing runtime test**

Require:
- `RuntimeBuilder().strictSchedulerCounts(true)` exists
- `ioSchedulerCount(4).computeSchedulerCount(0)` starts a runtime with exactly `4` IO schedulers and `0` compute schedulers
- default mode keeps existing auto-expand semantics

**Step 2: Verify RED**

Build and run only the new runtime test until it fails for the expected missing API / behavior reason.

### Task 2: Implement strict scheduler counts in `galay-kernel`

**Files:**
- Modify: `../galay-kernel/galay-kernel/kernel/Runtime.h`
- Modify: `../galay-kernel/galay-kernel/kernel/Runtime.cc`

**Step 1: Add opt-in config + builder method**

Implement a boolean flag such as `strict_scheduler_counts` in `RuntimeConfig` and a matching `RuntimeBuilder` setter.

**Step 2: Change scheduler materialization semantics**

Keep default behavior unchanged, but when strict mode is enabled, materialize exactly the configured IO/compute scheduler counts, including `0` compute schedulers.

**Step 3: Rebuild and run the runtime test**

Expected: PASS.

### Task 3: Thread strict mode through server builders

**Files:**
- Modify: `galay-http/kernel/http/HttpServer.h`
- Modify: `galay-http/kernel/http2/Http2Server.h`
- Add/modify a small compile-surface test in `test/` if useful

**Step 1: Add builder/config surface**

Expose `strictSchedulerCounts(bool = true)` on HTTP/HTTPS/H2c/H2 server builders and pass it into the runtime builder.

**Step 2: Keep compatibility**

Do not change default behavior for non-benchmark callers unless they explicitly opt in.

### Task 4: Enable fairness mode in benchmark binaries and print actual counts

**Files:**
- Modify: `benchmark/B5-WebsocketServer.cc`
- Modify: `benchmark/B7-WssServer.cc`
- Modify: `benchmark/B3-H2cServer.cc`
- Modify: `benchmark/B12-H2Server.cc`
- Optionally align: `benchmark/B1-HttpServer.cc`, `benchmark/B14-HttpsServer.cc`

**Step 1: Enable strict runtime counts**

Use:
- `.ioSchedulerCount(io_threads)`
- `.computeSchedulerCount(0)`
- `.strictSchedulerCounts(true)`

**Step 2: Print configured vs actual runtime counts**

After startup, print:
- configured IO / compute counts
- strict-mode enabled status
- actual `server.getRuntime().getIOSchedulerCount()` / `getComputeSchedulerCount()`

### Task 5: Re-baseline `ws` / `wss` / `h2c` / `h2`

**Files:**
- Output only: `benchmark/results/<timestamp>-strict-runtime-fairness/`

**Step 1: Rebuild impacted artifacts**

Rebuild updated kernel, install it for `galay-http`, then rebuild the four Galay benchmark binaries plus compare server/client artifacts if needed.

**Step 2: Run one-shot protocol comparisons**

Collect fresh results for:
- `ws`
- `wss`
- `h2c`
- `h2`

with the same workload shape as before.

**Step 3: Save the new baseline**

Record the aggregate and note that all future “distance to Rust 5%” judgments use this stricter baseline.

## Execution note

The user explicitly asked to implement and rerun now, so this plan is executed in the current session.
