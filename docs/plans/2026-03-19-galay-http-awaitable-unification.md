# galay-http Awaitable Unification Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 将 `galay-http` 的 Awaitable 实现统一到 `AwaitableBuilder` / `AwaitableBuilder::fromStateMachine(...)`，移除 HTTP 私有 awaitable 执行器，并完成回归、压测、文档和发布准备。

**Architecture:** HTTP/1.x 等线性协议流改为 builder facade，SSL / WebSocket message / HTTP/2 等复杂场景改为显式 state machine。高层 API 尽量保留原方法名和调用语义，但底层协议逻辑只允许落在 `Flow` / `Machine` / parse handler 里，执行逻辑完全交给 kernel/ssl 的共享 awaitable core。

**Tech Stack:** C++23、coroutines、`galay-kernel v3.2.x` `AwaitableBuilder` / `StateMachineAwaitable`、`galay-ssl` builder/state-machine、CMake、repo 自带 tests / benchmarks / docs。

---

### Task 1: 锁定公开表面和删除目标

**Files:**
- Modify: `test/T57-awaitable_builder_parser.cc`
- Modify: `test/T58-ws_pipeline_surface.cc`
- Create: `test/T59-http2_state_machine_surface.cc`
- Modify: `test/CMakeLists.txt`
- Inspect: `galay-http/kernel/InternalPipelineAwaitable.h`
- Inspect: `galay-http/kernel/SslRecvCompatAwaitable.h`

**Step 1: 写出失败测试，锁定最终公开表面**

在 `test/T57-awaitable_builder_parser.cc` 增加对 HTTP/1.x reader/writer 返回面“只依赖新 builder/state-machine 表面”的编译约束；在 `test/T58-ws_pipeline_surface.cc` 增加 WebSocket 公开 API 不再依赖 HTTP 私有 pipeline 执行器的约束；新增 `test/T59-http2_state_machine_surface.cc` 锁定 HTTP/2 连接级 awaitable 入口存在。

```cpp
static_assert(requires(HttpSession<TcpSocket>& session) {
    { session.getResponseAwaitable() };
});

static_assert(requires(Http2Conn<TcpSocket>& conn) {
    { conn.recvFrameAwaitable() };
});
```

**Step 2: 运行测试确认当前表面仍未完全满足目标**

Run: `cmake --build build-kv320-check --target T57-awaitable_builder_parser T58-ws_pipeline_surface T59-http2_state_machine_surface --parallel`
Expected: 新增断言或新测试在当前实现下失败，暴露仍依赖旧兼容层的路径。

**Step 3: 最小修改测试入口与 CMake**

修改 `test/CMakeLists.txt` 挂上新测试目标，只做必要增量，不提前修改业务实现。

```cmake
add_executable(T59-http2_state_machine_surface test/T59-http2_state_machine_surface.cc)
target_link_libraries(T59-http2_state_machine_surface PRIVATE GalayHttp)
```

**Step 4: 再次构建确认失败点稳定**

Run: `cmake --build build-kv320-check --target T59-http2_state_machine_surface --parallel`
Expected: 编译失败点固定在待迁移的旧 awaitable 表面。

**Step 5: 提交**

```bash
git add test/T57-awaitable_builder_parser.cc test/T58-ws_pipeline_surface.cc test/T59-http2_state_machine_surface.cc test/CMakeLists.txt
git commit -m "测试: 锁定 HTTP 新 Awaitable 公开表面"
```

### Task 2: 收敛 HTTP/1.x TcpSocket 线性 reader/writer

**Files:**
- Modify: `galay-http/kernel/http/HttpReader.h`
- Modify: `galay-http/kernel/http/HttpWriter.h`
- Modify: `galay-http/kernel/http/HttpSession.h`
- Modify: `test/T3-reader_writer_server.cc`
- Modify: `test/T4-reader_writer_client.cc`
- Modify: `test/T6-http_client_awaitable.cc`
- Modify: `test/T7-http_client_awaitable_edge_cases.cc`

**Step 1: 写出失败测试，覆盖 builder 化后的线性流**

先在 `T3/T4/T6/T7` 增加断言，锁定 request/response 读取在分段到达时能通过 `parse -> kNeedMore -> recv -> parse` 完成闭环，且对外方法名保持不变。

```cpp
auto result = co_await session.getResponseAwaitable();
EXPECT_TRUE(result.has_value());
EXPECT_TRUE(result.value().has_value());
```

**Step 2: 运行相关测试确认当前实现还存在厚 awaitable 依赖**

Run: `cmake --build build-kv320-check --target T3-reader_writer_server T4-reader_writer_client T6-http_client_awaitable T7-http_client_awaitable_edge_cases --parallel`
Expected: 至少一个目标因新的表面约束或内部类型调整而失败。

**Step 3: 在 `HttpReader.h` / `HttpWriter.h` 中提取共享 Flow 和 parse handler**

将 reader/writer 中与协议相关的累计字节、ring buffer window、错误映射、parse status 逻辑收敛进 `Flow`，把每次 builder 装配放到公开方法里现建现用。

```cpp
auto makeRequestReader() {
    return AwaitableBuilder<Result, 4, RequestReadFlow>(m_socket.controller(), m_flow)
        .recv<&RequestReadFlow::onRecv>(m_flow.buffer.data(), m_flow.buffer.size())
        .parse<&RequestReadFlow::onParse>()
        .finish<&RequestReadFlow::onFinish>()
        .build();
}
```

**Step 4: 运行相关测试确认线性流通过**

Run: `ctest --test-dir build-kv320-check -R 'T3-reader_writer_server|T4-reader_writer_client|T6-http_client_awaitable|T7-http_client_awaitable_edge_cases' --output-on-failure`
Expected: 全部 PASS。

**Step 5: 提交**

```bash
git add galay-http/kernel/http/HttpReader.h galay-http/kernel/http/HttpWriter.h galay-http/kernel/http/HttpSession.h test/T3-reader_writer_server.cc test/T4-reader_writer_client.cc test/T6-http_client_awaitable.cc test/T7-http_client_awaitable_edge_cases.cc
git commit -m "重构: 用 AwaitableBuilder 收敛 HTTP 一次性读写流"
```

### Task 3: 删除 HTTP/1.x SSL 兼容套娃并切到 SSL state machine

**Files:**
- Modify: `galay-http/kernel/http/HttpReader.h`
- Modify: `galay-http/kernel/http/HttpWriter.h`
- Modify: `galay-http/kernel/http/HttpSession.h`
- Delete: `galay-http/kernel/SslRecvCompatAwaitable.h`
- Modify: `test/T21-https_server.cc`
- Modify: `test/T22-https_client.cc`
- Modify: `test/T23-https_stress_test.cc`
- Modify: `test/T24-simple_https_test.cc`

**Step 1: 写出失败测试，锁定 WANT_READ/WANT_WRITE 与关闭路径**

在 HTTPS 测试里增加覆盖：分段响应、对端关闭、读后继续写、连接关闭后错误映射不变。

```cpp
EXPECT_EQ(result.error().code(), kConnectionClose);
```

**Step 2: 运行 HTTPS 测试确认当前实现的旧兼容层仍在路径上**

Run: `cmake --build build-kv320-check --target T21-https_server T22-https_client T23-https_stress_test T24-simple_https_test --parallel`
Expected: 至少一个用例在删除兼容层前失败或暴露旧依赖。

**Step 3: 将 SSL 路径改为直接使用 `galay-ssl` builder/state-machine**

删除 `CustomAwaitable + SslRecvCompatAwaitable + ProtocolRecvAwaitable` 套娃，把 `want_read` / `want_write`、读取、解析与完成逻辑显式表达为 machine 或由 SSL builder 线性装配。

```cpp
auto aw = AwaitableBuilder<Result>::fromStateMachine(
    m_socket.controller(),
    makeHttpsReadMachine()
).build();
```

**Step 4: 运行 HTTPS 回归确认行为一致**

Run: `ctest --test-dir build-kv320-check -R 'T21-https_server|T22-https_client|T23-https_stress_test|T24-simple_https_test' --output-on-failure`
Expected: 全部 PASS。

**Step 5: 提交**

```bash
git add galay-http/kernel/http/HttpReader.h galay-http/kernel/http/HttpWriter.h galay-http/kernel/http/HttpSession.h test/T21-https_server.cc test/T22-https_client.cc test/T23-https_stress_test.cc test/T24-simple_https_test.cc
git rm galay-http/kernel/SslRecvCompatAwaitable.h
git commit -m "重构: 用状态机统一 HTTPS 读写推进"
```

### Task 4: 收敛 WebSocket 读写与消息聚合

**Files:**
- Modify: `galay-http/kernel/websocket/WsReader.h`
- Modify: `galay-http/kernel/websocket/WsWriter.h`
- Modify: `galay-http/kernel/websocket/WsSession.h`
- Modify: `galay-http/kernel/websocket/WsClient.h`
- Modify: `test/T18-ws_server.cc`
- Modify: `test/T19-ws_client.cc`
- Modify: `test/T20-websocket_client.cc`
- Modify: `test/T58-ws_pipeline_surface.cc`
- Modify: `benchmark/B5-WebsocketServer.cc`
- Modify: `benchmark/B7-WssServer.cc`

**Step 1: 写出失败测试，锁定单帧 builder 和消息级 state-machine**

在 `T58` 和 `T18/T19/T20` 中增加约束：单帧读写对外保持稳定，消息聚合与 close/ping/pong 依然正确。

```cpp
auto msg = co_await session.readMessage();
EXPECT_TRUE(msg.has_value());
EXPECT_EQ(msg->payload(), "hello");
```

**Step 2: 运行相关测试确认旧 pipeline 依赖存在**

Run: `cmake --build build-kv320-check --target T18-ws_server T19-ws_client T20-websocket_client T58-ws_pipeline_surface --parallel`
Expected: 当前实现未完全满足“无 HTTP 私有 pipeline 执行器”目标。

**Step 3: 将 WebSocket 单帧与消息流拆开**

- 单帧收发改用 builder
- 消息聚合、close frame、控制帧穿插改用 state machine
- 移除对 `InternalPipelineAwaitable` 的依赖

```cpp
auto frame_aw = AwaitableBuilder<FrameResult, 4, FrameFlow>(controller, flow)
    .recv<&FrameFlow::onRecv>(flow.buffer.data(), flow.buffer.size())
    .parse<&FrameFlow::onParse>()
    .build();
```

**Step 4: 运行 WebSocket 回归和 benchmark smoke**

Run: `ctest --test-dir build-kv320-check -R 'T18-ws_server|T19-ws_client|T20-websocket_client|T58-ws_pipeline_surface' --output-on-failure`
Run: `cmake --build build-kv320-check --target B5-WebsocketServer B7-WssServer --parallel`
Expected: 回归 PASS，benchmark 可执行成功生成。

**Step 5: 提交**

```bash
git add galay-http/kernel/websocket/WsReader.h galay-http/kernel/websocket/WsWriter.h galay-http/kernel/websocket/WsSession.h galay-http/kernel/websocket/WsClient.h test/T18-ws_server.cc test/T19-ws_client.cc test/T20-websocket_client.cc test/T58-ws_pipeline_surface.cc benchmark/B5-WebsocketServer.cc benchmark/B7-WssServer.cc
git commit -m "重构: 收敛 WebSocket Awaitable 到 Builder 和状态机"
```

### Task 5: 收敛 HTTP/2 连接级读写和帧解析

**Files:**
- Modify: `galay-http/kernel/http2/Http2Conn.h`
- Modify: `galay-http/kernel/http2/H2cClient.h`
- Modify: `galay-http/kernel/http2/Http2ConnectionCore.h`
- Modify: `galay-http/kernel/http2/Http2ConnectionCore.cc`
- Modify: `galay-http/kernel/http2/Http2Server.h`
- Modify: `galay-http/kernel/http2/Http2StreamManager.h`
- Modify: `test/T25-h2c_client.cc`
- Modify: `test/T27-h2_server.cc`
- Modify: `test/T28-h2_client.cc`
- Modify: `test/T42-h2_active_conn_api.cc`
- Modify: `test/T43-h2_active_conn_preferred.cc`
- Modify: `test/T44-h2c_client_shutdown.cc`
- Modify: `test/T46-h2_active_conn_retire.cc`
- Create: `test/T59-http2_state_machine_surface.cc`

**Step 1: 写出失败测试，锁定 HTTP/2 state-machine 表面和关键行为**

覆盖 preface、settings、active conn 切换、retire/shutdown、分片帧解析和连续收帧。

```cpp
auto result = co_await conn.recvFramesAwaitable();
EXPECT_TRUE(result.has_value());
EXPECT_FALSE(result->empty());
```

**Step 2: 运行 HTTP/2 测试确认当前实现仍在使用过渡执行器**

Run: `cmake --build build-kv320-check --target T25-h2c_client T27-h2_server T28-h2_client T42-h2_active_conn_api T43-h2_active_conn_preferred T44-h2c_client_shutdown T46-h2_active_conn_retire T59-http2_state_machine_surface --parallel`
Expected: 新的 surface/行为测试失败，暴露旧执行骨架。

**Step 3: 把连接级持续收发改成显式 machine**

移除连接对象内部的 HTTP 私有调度器，用 machine 表达“本地动作、等待读、等待写、完成或失败”。

```cpp
struct H2ConnMachine {
    using result_type = std::expected<FrameBatch, Http2ErrorCode>;
    auto operator()() -> MachineAction<result_type>;
};
```

**Step 4: 运行 HTTP/2 回归**

Run: `ctest --test-dir build-kv320-check -R 'T25-h2c_client|T27-h2_server|T28-h2_client|T42-h2_active_conn_api|T43-h2_active_conn_preferred|T44-h2c_client_shutdown|T46-h2_active_conn_retire|T59-http2_state_machine_surface' --output-on-failure`
Expected: 全部 PASS。

**Step 5: 提交**

```bash
git add galay-http/kernel/http2/Http2Conn.h galay-http/kernel/http2/H2cClient.h galay-http/kernel/http2/Http2ConnectionCore.h galay-http/kernel/http2/Http2ConnectionCore.cc galay-http/kernel/http2/Http2Server.h galay-http/kernel/http2/Http2StreamManager.h test/T25-h2c_client.cc test/T27-h2_server.cc test/T28-h2_client.cc test/T42-h2_active_conn_api.cc test/T43-h2_active_conn_preferred.cc test/T44-h2c_client_shutdown.cc test/T46-h2_active_conn_retire.cc test/T59-http2_state_machine_surface.cc
git commit -m "重构: 用状态机统一 HTTP2 连接级 Awaitable"
```

### Task 6: 删除 HTTP 私有 awaitable 执行器并清理残留公开面

**Files:**
- Delete: `galay-http/kernel/InternalPipelineAwaitable.h`
- Modify: `galay-http/kernel/http/HttpReader.h`
- Modify: `galay-http/kernel/http/HttpSession.h`
- Modify: `galay-http/kernel/websocket/WsReader.h`
- Modify: `galay-http/kernel/http2/Http2Conn.h`
- Modify: `galay-http/module/ModulePrelude.hpp`
- Modify: `galay-http/CMakeLists.txt`

**Step 1: 写出失败测试，确认不再依赖旧执行器**

补充或更新 surface tests，确保公开头文件和模块导出不再暴露 `InternalPipelineAwaitable`、`CustomAwaitable` 兼容层。

```cpp
static_assert(!requires { typename galay::http::InternalPipelineAwaitable<4>; });
```

**Step 2: 运行编译面测试确认当前仍有引用**

Run: `cmake --build build-kv320-check --target T57-awaitable_builder_parser T58-ws_pipeline_surface T59-http2_state_machine_surface --parallel`
Expected: 删除前至少有一个目标因残留引用失败。

**Step 3: 删除旧执行器并修正导出**

删除文件与引用，更新模块导出与 CMake 源列表，确保外部只能见到 builder/state-machine 入口。

```cpp
// no InternalPipelineAwaitable include
```

**Step 4: 重新运行编译面测试**

Run: `ctest --test-dir build-kv320-check -R 'T57-awaitable_builder_parser|T58-ws_pipeline_surface|T59-http2_state_machine_surface' --output-on-failure`
Expected: 全部 PASS。

**Step 5: 提交**

```bash
git rm galay-http/kernel/InternalPipelineAwaitable.h
git add galay-http/kernel/http/HttpReader.h galay-http/kernel/http/HttpSession.h galay-http/kernel/websocket/WsReader.h galay-http/kernel/http2/Http2Conn.h galay-http/module/ModulePrelude.hpp galay-http/CMakeLists.txt
git commit -m "清理: 移除 HTTP 私有 Awaitable 执行器"
```

### Task 7: 更新示例、文档和基准

**Files:**
- Modify: `docs/README.md`
- Modify: `docs/02-API参考.md`
- Modify: `docs/03-使用指南.md`
- Modify: `docs/04-示例代码.md`
- Modify: `docs/05-性能测试.md`
- Modify: `README.md`
- Modify: `examples/include/E3-websocket_server.cpp`
- Modify: `examples/include/E7-wss_server.cpp`
- Modify: `benchmark/B4-H2cClient.cc`
- Modify: `benchmark/B5-WebsocketServer.cc`
- Modify: `benchmark/B7-WssServer.cc`

**Step 1: 写出失败文档检查点**

列出仓库里所有旧 `CustomAwaitable`、`InternalPipelineAwaitable`、或过时调用示例，作为必须清零的检查项。

```bash
rg -n "CustomAwaitable|InternalPipelineAwaitable|SslRecvCompatAwaitable|Coroutine" README.md docs examples benchmark galay-http
```

**Step 2: 运行搜索确认旧文档/示例仍存在**

Run: `rg -n "CustomAwaitable|InternalPipelineAwaitable|SslRecvCompatAwaitable|Coroutine" README.md docs examples benchmark galay-http`
Expected: 返回待清理结果列表。

**Step 3: 最小更新文档和示例**

把所有面向用户的例子统一改成 builder/state-machine 写法，并在性能文档中增加本轮改造后的 benchmark 记录位置。

```cpp
auto aw = AwaitableBuilder<Result, 4, Flow>(controller, flow)
    .recv<&Flow::onRecv>(...)
    .parse<&Flow::onParse>()
    .build();
```

**Step 4: 重新运行搜索确认旧写法已清零**

Run: `rg -n "CustomAwaitable|InternalPipelineAwaitable|SslRecvCompatAwaitable|Coroutine" README.md docs examples benchmark galay-http`
Expected: 无结果，或仅剩 changelog / plan 文档中的历史说明。

**Step 5: 提交**

```bash
git add README.md docs/README.md docs/02-API参考.md docs/03-使用指南.md docs/04-示例代码.md docs/05-性能测试.md examples/include/E3-websocket_server.cpp examples/include/E7-wss_server.cpp benchmark/B4-H2cClient.cc benchmark/B5-WebsocketServer.cc benchmark/B7-WssServer.cc
git commit -m "文档: 更新 HTTP 新 Awaitable 用法与性能说明"
```

### Task 8: 全量回归、压测、提交与发布准备

**Files:**
- Modify: `docs/05-性能测试.md`
- Modify: `README.md`
- Create: `docs/releases/<new-major-version>.md`
- Create: `test_results/kernel-v320-rollout-2026-03-18/galay-http/...` (external archive path)

**Step 1: 运行全量构建和回归**

Run: `cmake -S . -B build-kv320-check -DCMAKE_BUILD_TYPE=Release -DGALAY_HTTP_BUILD_TESTS=ON -DGALAY_HTTP_BUILD_BENCHMARKS=ON -DCMAKE_PREFIX_PATH='/Users/gongzhijie/Desktop/projects/git/test_results/kernel-v320-rollout-2026-03-18/prefix'`
Run: `cmake --build build-kv320-check --parallel`
Run: `ctest --test-dir build-kv320-check --output-on-failure`
Expected: 全部测试 PASS，允许明确标注的已知 skip。

**Step 2: 运行压测并归档结果**

Run: `./build-kv320-check/bin/B4-H2cClient`
Run: `./build-kv320-check/bin/B5-WebsocketServer`
Run: `./build-kv320-check/bin/B7-WssServer`
Expected: 输出吞吐/延迟数据，可与改造前结果对比，并归档到 rollout 结果目录。

**Step 3: 更新 release 文案和版本记录**

编写 release notes，明确说明：

- 旧 HTTP 私有 awaitable 执行器退场
- HTTP 线性流统一到 `AwaitableBuilder`
- HTTPS / WS / HTTP2 复杂流统一到 state machine
- 用户若自定义协议，应优先提供 flow 或 machine，而不是自定义 awaitable 类

```markdown
## Breaking Changes
- 移除 HTTP 私有 Awaitable 执行器
- 旧兼容 awaitable 类型不再作为扩展入口
```

**Step 4: 中文提交并打大版本 tag**

```bash
git add .
git commit -m "重构: 全面收敛 galay-http Awaitable 到 Builder 与状态机"
git tag -a v4.0.0 -m "galay-http v4.0.0：统一 Awaitable 到 Builder 和状态机"
```

**Step 5: 发布前复核**

Run: `git status --short`
Run: `git show --stat --oneline HEAD`
Run: `git tag --list 'v4.*'`
Expected: 工作树干净，提交与 tag 正确，release 文案可发布。
