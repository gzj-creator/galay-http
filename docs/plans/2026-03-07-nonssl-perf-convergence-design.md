# Non-SSL Performance Convergence Design

**Date:** 2026-03-07

**Goal:** Use the updated `galay-kernel` scheduler/runtime as the new baseline, then continue optimizing non-SSL `http` / `ws` / `h2c` so galay-http reaches at least 90% of the comparable Go/Rust throughput without a meaningful `P99` regression.

**Scope:** This design only covers non-SSL protocols:

- `http`
- `ws`
- `h2c`

`https`, `wss`, and `h2` are intentionally out of scope because TLS work is being optimized separately.

## Background

The optimization order is:

1. `h2c`
2. `http`
3. `ws`

The success target is:

- service-side first
- client-side convergence in the same pass
- compare against the existing Go/Rust benchmark matrix
- accept the result once galay-http reaches at least 90% of the peer baseline for throughput, while not materially worsening tail latency

This ordering matches the repository’s current state:

- the freshest experiments, benchmark result directories, and design notes are concentrated in `h2c`
- `http` already has prior parsing/header fast-path work that can be extended into response and client send paths
- `ws` can reuse much of the `http` and socket-level fast path once the upgrade is complete

There is one important new variable: `galay-kernel` has already been optimized again. That means any old hotspot ranking in galay-http may now be stale. Before changing protocol code, the new runtime must be re-baselined so the remaining gaps are real, current, and worth chasing.

## Current Situation

The checked-in benchmark history already shows that `h2c` moved from a clearly broken state to a competitive one:

- `benchmark/results/20260307-094000-h2c-current-gap-check/aggregate.csv` shows galay at `17.5k rps`
- `benchmark/results/20260307-095056-h2c-after-retire-fix/aggregate.csv` shows galay at about `83.5k rps`
- `benchmark/results/20260307-111357-h2c-current-one-shot/aggregate.csv` shows galay at about `73.0k rps`, Go at about `62.2k rps`, Rust at about `100.8k rps`

This is enough evidence to justify a narrow strategy:

- do not assume the runtime is still the dominant bottleneck
- do not start with a large scheduler/runtime rewrite inside galay-http
- do re-measure with the updated kernel, then optimize the remaining protocol-local hot paths

## Approaches Considered

### Approach A: Protocol hotspots first

Re-measure on the updated kernel, then continue attacking protocol-local hot paths in order: `h2c -> http -> ws`.

**Pros**

- fits the existing benchmark and code context
- fastest path to measurable wins
- smallest risk of colliding with ongoing SSL work

**Cons**

- shared runtime opportunities are only taken if they are still visible after re-baselining

### Approach B: Common runtime layer first

Treat the new kernel as an invitation to do a broad convergence pass across awaitables, wakeups, read/write helpers, and scheduler coupling before touching individual protocols.

**Pros**

- potentially benefits all protocols together
- could raise the long-term ceiling

**Cons**

- largest change surface
- most difficult to verify
- highest chance of optimizing the wrong layer if the new kernel already removed the prior bottleneck

### Approach C: Two-track protocol + runtime work

Run one short `h2c` hotspot track while simultaneously reshaping the common non-SSL data path.

**Pros**

- balances short-term and medium-term gains

**Cons**

- highest coordination cost
- easiest way to mix unrelated changes and lose attribution of performance wins

## Chosen Approach

Choose **Approach A with an explicit re-baseline phase**:

1. Re-baseline non-SSL protocols on the updated `galay-kernel`
2. Optimize `h2c` first until it is stably competitive
3. Carry the verified fast-path ideas into `http`
4. Finish with `ws`
5. Only make cross-protocol runtime changes when the new baseline proves a shared bottleneck still dominates

This keeps the work aligned with the current repository evidence while still letting the new kernel improvements surface naturally.

## Proposed Architecture

### Phase 0: Re-baseline on the updated kernel

Rebuild and re-run the existing non-SSL benchmark matrix with the updated `galay-kernel`:

- service-side numbers first
- client-side numbers checked in the same run
- compare against the existing Go/Rust harness

The outcome of this phase is not a code change; it is a new hotspot ranking. No protocol optimization should start until this ranking is trusted.

### Phase 1: `h2c` convergence

Focus on the `Http2StreamManager` and adjacent `h2c` client/server hot paths:

- reduce writer-loop orchestration overhead
- avoid extra `iovec` export/copy work
- reduce unnecessary wake/defer cycles in active stream handling
- flatten small awaitable chains where the updated kernel now makes direct paths cheaper
- keep HTTP/2 semantics unchanged: frame ordering, flow control, stream lifecycle, and shutdown behavior stay intact

This phase targets both server and client, but server throughput remains the first acceptance gate.

### Phase 2: `http` convergence

Apply the same style of optimization to `http`:

- extend prior header parsing/header fast-path work into response emission
- reduce default-header normalization and repeated small-string work
- lower response send fragmentation and coroutine boundary count
- make sure the benchmark client path does not become the new bottleneck

The guiding rule is reuse: if a fast path already proved out in `h2c`, prefer adapting it over inventing a new one for `http`.

### Phase 3: `ws` convergence

Optimize the steady-state WebSocket message path after upgrade:

- reduce per-frame parse/build overhead for small echo messages
- minimize mask/header/payload descriptor churn
- avoid redundant buffering once a connection is already in WebSocket mode
- keep ping/pong/close semantics unchanged

This phase intentionally reuses the `http` and socket-level improvements rather than creating a separate buffering model just for `ws`.

## Cross-Protocol Principles

- **Server-first, client-validated:** optimize the server first, but verify the client is not hiding server gains.
- **One hotspot at a time:** only one dominant hotspot should be targeted in each pass.
- **No semantic shortcuts:** protocol ordering, shutdown semantics, and correctness stay locked by tests.
- **Measure before generalizing:** a shared optimization only becomes protocol-common after it proves useful in one protocol first.
- **Non-SSL only:** do not mix TLS work into this branch of optimization.

## Data Flow

Each optimization loop follows the same sequence:

1. Rebuild with the updated kernel
2. Run the relevant benchmark
3. Inspect the hottest remaining path
4. Add or tighten a focused behavioral test
5. Apply the minimal hot-path change
6. Re-run tests
7. Re-run the benchmark
8. Record the result before moving to the next protocol

That keeps every performance claim tied to a concrete code change and a concrete benchmark delta.

## Verification Strategy

Every protocol phase needs both correctness and performance verification.

### Correctness

- `h2c`: active-conn, shutdown, awaitable-surface, and stream lifecycle tests
- `http`: reader/writer, client awaitable, header fast-path, and header case handling tests
- `ws`: server/client upgrade, echo, ping/pong, and close tests

### Performance

- reuse the existing benchmark programs under `benchmark/`
- compare galay against the existing Go/Rust harness
- record outputs under `benchmark/results/<timestamp>-.../`
- require at least one focused benchmark rerun after each phase

## Success Criteria

The work is successful when:

- `h2c`, `http`, and `ws` all have fresh non-SSL benchmark results on the updated kernel
- galay-http reaches at least 90% of the corresponding Go/Rust baseline for the chosen benchmark shape
- server-side gains are not invalidated by client-side bottlenecks
- no correctness regression appears in the targeted protocol tests
- tail latency does not regress in a clearly material way while throughput improves

## Out of Scope

- `https`, `wss`, `h2`
- TLS handshake, ALPN, certificate handling, and SSL library changes
- broad scheduler/runtime redesign inside galay-http without fresh benchmark evidence
- speculative refactors that do not have a measured hotspot behind them

## Notes for Execution

- Execute the implementation in a dedicated worktree if code changes begin.
- Do not overwrite the current experimental uncommitted files blindly; revalidate them against the new kernel baseline first.
- No commit is included in this design handoff because commits should only happen if explicitly requested.
