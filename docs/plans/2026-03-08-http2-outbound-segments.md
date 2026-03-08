# HTTP/2 Outbound Segments Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace HTTP/2 hot-path full-frame string serialization with a segmented outbound representation that improves `h2c` send performance and prepares the same path for `h2`.

**Architecture:** Keep protocol semantics and control flow intact, but change the writer input model from “serialized frame bytes only” to “serialized compatibility packets + segmented header/payload packets”. Switch `HEADERS` and `DATA` to the segmented model, keep fallback flattening for SSL-backed writes, and validate with existing H2 regression tests plus a new focused packet-equivalence test.

**Tech Stack:** C++23, HTTP/2 frame builders, `Http2Stream`, `Http2StreamManager`, `IoVecWriteState`, existing H2 contract tests, benchmark `B3-H2cServer`.

---

### Task 1: Lock the segmented outbound surface

**Files:**
- Create: `test/T49-h2_outbound_segments.cc`
- Modify: `test/T33-h2_stream_frame_api.cc`
- Modify: `test/T37-h2_frame_builder_bytes.cc`

**Step 1: Write the failing test**

Add a focused regression that requires:

- an outbound packet type that can represent segmented frame output
- frame-header-only builder helpers for DATA and HEADERS
- a move-based fast-path send surface on `Http2Stream`

Extend existing tests to require:

- `Http2Stream::sendData(std::string&&, bool)`
- `Http2Stream::replyData(std::string&&, bool)`
- segmented header helper bytes flatten to the same output as the old builder helpers

**Step 2: Run test to verify it fails**

Run:

```bash
cmake --build build-test --target T33-h2_stream_frame_api T37-h2_frame_builder_bytes T49-h2_outbound_segments -j4
```

Expected: compile failure because the new segmented helpers/surfaces do not exist yet.

**Step 3: Write minimal implementation**

Add just enough declarations and helpers so the new tests can compile:

- segmented packet representation skeleton
- header-bytes builder helpers
- move-based send/reply overload declarations

**Step 4: Run test to verify it passes**

Run:

```bash
./build-test/test/T33-h2_stream_frame_api
./build-test/test/T37-h2_frame_builder_bytes
./build-test/test/T49-h2_outbound_segments
```

Expected: PASS.

### Task 2: Implement segmented packet ownership and byte equivalence

**Files:**
- Modify: `galay-http/kernel/http2/Http2Stream.h`
- Modify: `galay-http/protoc/http2/Http2Frame.h`
- Modify: `galay-http/protoc/http2/Http2Frame.cc`
- Test: `test/T49-h2_outbound_segments.cc`
- Test: `test/T37-h2_frame_builder_bytes.cc`

**Step 1: Write/extend the failing test**

Require:

- segmented packets can hold a 9-byte frame head and owned payload
- flattening a segmented packet yields the exact same bytes as the current full-frame builders
- serialized compatibility packets still flatten unchanged

**Step 2: Run test to verify it fails**

Run:

```bash
./build-test/test/T37-h2_frame_builder_bytes
./build-test/test/T49-h2_outbound_segments
```

Expected: FAIL until ownership/flatten behavior is implemented.

**Step 3: Write minimal implementation**

Implement:

- packet ownership state for serialized vs segmented forms
- low-level frame-header-only byte builders
- flatten helper used by tests and later by SSL fallback

**Step 4: Run test to verify it passes**

Run:

```bash
./build-test/test/T37-h2_frame_builder_bytes
./build-test/test/T49-h2_outbound_segments
```

Expected: PASS.

### Task 3: Teach writerLoop to consume segmented packets

**Files:**
- Modify: `galay-http/kernel/http2/Http2StreamManager.h`
- Test: `test/T43-h2_active_conn_preferred.cc`
- Test: `test/T44-h2c_client_shutdown.cc`
- Test: `test/T46-h2_active_conn_retire.cc`
- Test: `test/T49-h2_outbound_segments.cc`

**Step 1: Write/extend the failing test**

If needed, extend `T49` to assert the segmented packet exposes the expected iovec shape (one segment for serialized fallback, two segments for header+payload).

**Step 2: Run test to verify it fails**

Run:

```bash
cmake --build build-test --target T43-h2_active_conn_preferred T44-h2c_client_shutdown T46-h2_active_conn_retire T49-h2_outbound_segments -j4
./build-test/test/T49-h2_outbound_segments
```

Expected: FAIL if the writer-facing segmented expansion contract is not satisfied yet.

**Step 3: Write minimal implementation**

Update `writerLoop()` to:

- collect outbound packets instead of only serialized strings
- expand segmented packets into `iovec {header, payload}`
- keep partial-write progress logic unchanged
- use flatten-on-demand only for the non-`writev` fallback path

**Step 4: Run test to verify it passes**

Run:

```bash
./build-test/test/T43-h2_active_conn_preferred
./build-test/test/T44-h2c_client_shutdown
./build-test/test/T46-h2_active_conn_retire
./build-test/test/T49-h2_outbound_segments
```

Expected: PASS.

### Task 4: Switch HEADERS and DATA hot paths

**Files:**
- Modify: `galay-http/kernel/http2/Http2Stream.h`
- Modify: `benchmark/B3-H2cServer.cc`
- Modify: `benchmark/B12-H2Server.cc`
- Test: `test/T33-h2_stream_frame_api.cc`
- Test: `test/T42-h2_active_conn_api.cc`
- Test: `test/T48-non_ssl_kernel_surface.cc`

**Step 1: Write/extend the failing test**

Lock the move-based fast path and keep the public compatibility overloads intact.

**Step 2: Run test to verify it fails**

Run:

```bash
cmake --build build-test --target T33-h2_stream_frame_api T42-h2_active_conn_api T48-non_ssl_kernel_surface -j4
```

Expected: FAIL until `Http2Stream` switches the hot send path to segmented packets with move support.

**Step 3: Write minimal implementation**

Change:

- `sendHeadersInternal()` to queue `{header, owned_hpack_block}`
- `sendDataInternal(const std::string&)` to use segmented owned payload
- `sendDataInternal(std::string&&)` and matching reply overloads to move payload into the outbound packet
- benchmark echo handlers to use move-based body transfer where semantically safe

**Step 4: Run test to verify it passes**

Run:

```bash
./build-test/test/T33-h2_stream_frame_api
./build-test/test/T42-h2_active_conn_api
./build-test/test/T48-non_ssl_kernel_surface
```

Expected: PASS.

### Task 5: Verify the H2 regression set

**Files:**
- No additional source files

**Step 1: Build the targeted regression set**

Run:

```bash
cmake --build build-test --target \
  T33-h2_stream_frame_api \
  T37-h2_frame_builder_bytes \
  T42-h2_active_conn_api \
  T43-h2_active_conn_preferred \
  T44-h2c_client_shutdown \
  T46-h2_active_conn_retire \
  T48-non_ssl_kernel_surface \
  T49-h2_outbound_segments \
  -j4
```

**Step 2: Run the full targeted regression set**

Run:

```bash
./build-test/test/T33-h2_stream_frame_api
./build-test/test/T37-h2_frame_builder_bytes
./build-test/test/T42-h2_active_conn_api
./build-test/test/T43-h2_active_conn_preferred
./build-test/test/T44-h2c_client_shutdown
./build-test/test/T46-h2_active_conn_retire
./build-test/test/T48-non_ssl_kernel_surface
./build-test/test/T49-h2_outbound_segments
```

Expected: PASS.

### Task 6: Re-baseline `h2c` and prepare `h2`

**Files:**
- Modify if needed: `benchmark/B12-H2Server.cc`

**Step 1: Build benchmark targets**

Run:

```bash
cmake --build build-test --target B3-H2cServer B4-H2cClient -j4
cmake --build build-ssl --target B12-H2Server B13-H2Client -j4
```

**Step 2: Re-run fresh `h2c` benchmark**

Run the same one-shot benchmark used in the current session:

```bash
./build-test/benchmark/B3-H2cServer 39080 4 0
benchmark/compare/protocols/go-client/go-proto-client-bin --proto h2c --addr 127.0.0.1:39080 --path /echo --conns 140 --duration 8 --size 128
```

Record results under:

- `benchmark/results/<timestamp>-h2c-outbound-segments/`

**Step 3: Sanity-check TLS-backed `h2` build/runtime**

Run:

```bash
./build-ssl/benchmark/B12-H2Server 39445 4 test/test.crt test/test.key
```

Then run the existing benchmark client against it.

**Step 4: Compare to current session baseline**

Compare against:

- `benchmark/results/20260308-020334-h2c-deferred-baseline`
- `benchmark/results/20260308-020514-h2c-deferred-after`
- `benchmark/results/20260308-020543-h2c-deferred-after-repeat`

## Execution note

This session is continuing in-place rather than opening a separate execution session, because the user explicitly asked to continue immediately.

## Commit note

No commit steps are included because commits were not requested in this session.
