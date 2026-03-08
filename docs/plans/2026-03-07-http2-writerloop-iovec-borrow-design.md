# HTTP/2 WriterLoop IoVec Borrow Design

**Goal:** Reduce `h2c` writer-path orchestration overhead by removing the extra `submit_iovecs` export/copy step inside `Http2StreamManager::writerLoop`, while keeping public `socket.writev(...)` behavior unchanged.

**Scope:** Only the internal `writerLoop` hot path in `galay-http`. Public `TcpSocket::writev(std::span<const iovec>)` remains unchanged. `H2cClient::ProtocolSendAwaitable` is not changed in this step.

## Current Problem

The current writer path builds `iovecs`, then constructs an `IoVecCursor` with a copied descriptor array, then exports another `submit_iovecs` vector on every partial-write loop before calling `socket.writev(...)`.

That means the hot path pays for:

- `iovecs` construction in `writerLoop`
- `IoVecCursor` internal descriptor copy
- `submit_iovecs` export on every loop iteration

The payload bytes are not copied, but descriptor churn is still unnecessary on a path that runs for every HTTP/2 response write.

## Proposed Change

Introduce a borrowed cursor utility that operates directly on a caller-owned mutable `iovec` span.

Properties:

- Owns no descriptor storage
- Advances in place by mutating the provided `iovec` array
- Exposes `data()`, `count()`, `empty()`, and `advance()` like the existing cursor

Then update `Http2StreamManager::writerLoop` to:

- build `iovecs` once
- create a borrowed cursor over `iovecs`
- call `socket.writev(std::span<const iovec>(cursor.data(), cursor.count()))`
- advance the borrowed cursor directly after each partial write

This removes the extra export vector and avoids the cursor’s internal descriptor copy in this path.

## Safety Boundary

This borrowed cursor is only valid while the underlying `iovec` storage stays alive.

That is safe in `writerLoop` because:

- `iovecs` is a coroutine local
- it remains alive across `co_await`
- the cursor never escapes the local loop

No public borrowed `writev` API is introduced in this step.

## Verification

- Add a unit-style test for the borrowed cursor that proves `advance()` mutates the original `iovec` descriptors in place.
- Re-run active-conn regressions: `T43`, `T44`, `T46`
- Rebuild `B3-H2cServer`
- Re-run `h2c` benchmark against current inline baseline
