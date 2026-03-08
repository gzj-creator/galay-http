# SSL Protocol Baseline Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Re-baseline `https`, `wss`, and `h2` performance on the current branch, then rank the SSL hotspots so the next optimization pass targets the biggest verified gap first.

**Architecture:** Reuse the existing benchmark compare harness shape, but limit execution to SSL protocols only. Build fresh Galay, Go, Rust server/client binaries, run one-shot comparisons for `https`, `wss`, and `h2`, then inspect the worst Galay SSL protocol with a sampling profile to identify the dominant remaining cost.

**Tech Stack:** C++23 benchmarks in `benchmark/`, Go compare client/server in `benchmark/compare/protocols/go-*`, Rust compare server in `benchmark/compare/protocols/rust-server`, `sample`, `inferno-collapse-sample`, `inferno-flamegraph`.

---

### Task 1: Refresh benchmark artifacts

**Files:**
- Modify: none
- Build dir: `build-ssl-nolog`
- Tools: `benchmark/compare/protocols/go-server`, `benchmark/compare/protocols/go-client`, `benchmark/compare/protocols/rust-server`

**Step 1: Reconfigure Galay SSL benchmark build**

Run:

```bash
cmake -S . -B build-ssl-nolog -DCMAKE_BUILD_TYPE=Release -DGALAY_HTTP_ENABLE_SSL=ON -DGALAY_HTTP_DISABLE_FRAMEWORK_LOG=ON
```

**Step 2: Build Galay SSL benchmark targets**

Run:

```bash
cmake --build build-ssl-nolog --target B7-WssServer B8-WssClient B12-H2Server B13-H2Client B14-HttpsServer -j4
```

**Step 3: Rebuild Go/Rust compare binaries**

Run:

```bash
go -C benchmark/compare/protocols/go-server build -o benchmark/compare/protocols/go-server/go-proto-server-bin .
go -C benchmark/compare/protocols/go-client build -o benchmark/compare/protocols/go-client/go-proto-client-bin .
(cd benchmark/compare/protocols/rust-server && cargo build --release)
```

### Task 2: Run SSL-only comparison matrix

**Files:**
- Output: `benchmark/results/<timestamp>-ssl-proto-baseline/`

**Step 1: Run `https` one-shot compare**

Start Galay/Go/Rust servers separately and use the shared Go benchmark client against:

- `127.0.0.1:39443` for Galay
- `127.0.0.1:19443` for Go
- `127.0.0.1:29443` for Rust

with:

- `proto=https`
- `path=/`
- `conns=160`
- `duration=8`
- `size=128`

**Step 2: Run `wss` one-shot compare**

Use:

- `proto=wss`
- `path=/ws`
- `conns=60`
- `duration=8`
- `size=1024`

**Step 3: Run `h2` one-shot compare**

Use:

- `proto=h2`
- `path=/echo`
- `conns=140`
- `duration=8`
- `size=128`

**Step 4: Aggregate results**

Write:

- `aggregate.csv`
- per-run client logs
- per-server logs

### Task 3: Profile the worst Galay SSL protocol

**Files:**
- Output: `benchmark/results/<timestamp>-ssl-proto-baseline/galay_<proto>.sample.txt`

**Step 1: Pick the largest verified gap**

Select the Galay SSL protocol with the worst ratio against the stronger of Go/Rust in the fresh baseline.

**Step 2: Run one sampled Galay benchmark**

Use `sample <pid> 5 -file ...` during the benchmark run.

**Step 3: Collapse stacks**

Run:

```bash
inferno-collapse-sample galay.sample.txt > galay.folded
inferno-flamegraph galay.folded > galay.flame.svg
```

### Task 4: Produce hotspot ranking

**Files:**
- Output: benchmark result logs only

**Step 1: Rank protocols**

For `https`, `wss`, `h2`, report:

- Galay RPS
- Go RPS
- Rust RPS
- gap to best baseline

**Step 2: Summarize likely next target**

Use the fresh sample plus numbers to recommend the next optimization target.

## Execution note

The user explicitly asked to start immediately, so this plan is executed in the current session.
