# WSS SSL Hot Path Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Identify and reduce the dominant steady-state `wss` hot-path overhead so Galay narrows the current gap versus Go/Rust under SSL.

**Architecture:** Keep the HTTPS upgrade flow unchanged, but profile the post-upgrade echo loop under SSL load. Focus on per-frame steady-state costs first: frame parsing/copying, payload ownership churn, response serialization, and SSL send/flush behavior. Prefer changes that also benefit library `WssWriter`/`WssReader` paths instead of benchmark-only tricks, unless sampling shows the benchmark echo loop itself dominates measured throughput.

**Tech Stack:** C++23 in `galay-http/kernel/websocket/` and `galay-http/protoc/websocket/`, benchmark targets in `benchmark/`, SSL-enabled build in `build-ssl-nolog`, macOS `sample` profiler.

---

### Task 1: Confirm the current hottest `wss` path

**Files:**
- Output only: `benchmark/results/<timestamp>-wss-sample/`

**Step 1: Build or reuse SSL benchmark artifacts**

Use `build-ssl-nolog` and ensure `B7-WssServer` and the compare client are fresh.

**Step 2: Run one sampled Galay `wss` benchmark**

Start `build-ssl-nolog/benchmark/B7-WssServer` with test certs, then run the shared compare client with the same `wss` workload used in the SSL baseline.

**Step 3: Collapse and inspect stacks**

Generate folded stacks / flamegraph if tools are available, otherwise inspect the raw `sample` output and rank the hottest functions.

### Task 2: Lock the optimization contract with tests

**Files:**
- Modify or add the smallest relevant test in `test/`

**Step 1: Add a failing regression or API-surface test**

Cover the chosen optimization contract, for example:
- segmented WebSocket frame output preserves exact bytes
- move-based send path avoids payload copy while keeping public overloads
- parser fast path handles contiguous input without intermediate container churn

**Step 2: Verify RED**

Build and run only the targeted test until it fails for the expected reason.

### Task 3: Implement the smallest library-level hot-path reduction

**Files:**
- Likely `galay-http/kernel/websocket/WsWriter.h`
- Possibly `galay-http/protoc/websocket/WebSocketFrame.h`
- Possibly `galay-http/protoc/websocket/WebSocketFrame.cc`
- Only touch `benchmark/B7-WssServer.cc` if profiling proves benchmark-local churn dominates reported throughput

**Step 1: Remove the measured hot allocation/copy path**

Prefer one of:
- header/payload segmented send for SSL writer path
- parser cursor path that avoids temporary `std::vector<iovec>` or `std::string::erase`
- move-based payload retention instead of frame rebuild + full serialization

**Step 2: Preserve protocol behavior**

Keep mask semantics, close/ping/pong handling, UTF-8 validation rules, and partial-write correctness unchanged.

### Task 4: Verify build, regression, and benchmark delta

**Files:**
- Output only

**Step 1: Build the smallest affected targets**

At minimum, rebuild the targeted test plus affected SSL benchmark targets.

**Step 2: Run targeted tests**

Run the exact tests added/modified and adjacent websocket/SSL protocol tests.

**Step 3: Re-run Galay `wss` benchmark**

Compare the new result against the fresh baseline (`107264.75 rps`) and report the delta.

## Execution note

The user explicitly asked to continue immediately, so this plan is executed in the current session.
