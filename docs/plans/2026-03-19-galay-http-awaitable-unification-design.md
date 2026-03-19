# galay-http Awaitable 收敛设计

**日期：** 2026-03-19

**目标：** 将 `galay-http` 全面收敛到 `galay-kernel v3.2.x` 的新 Awaitable 组合面，线性协议流统一使用 `AwaitableBuilder`，复杂双向协议统一使用 `AwaitableBuilder::fromStateMachine(...)`，不再继续扩散 `CustomAwaitable` 或 HTTP 私有执行器。

## 1. 背景

`galay-kernel` 在 `v3.2.x` 后已经明确区分两类推荐用法：

- 线性组合步骤优先使用 `AwaitableBuilder`
- 复杂双向协议、读写切换、handshake/shutdown 等场景优先使用 `AwaitableBuilder::fromStateMachine(...)`

`galay-http` 当前工作树虽然已经完成一轮兼容迁移，但仍然残留两类过渡实现：

- HTTP/1.x 读写路径里仍有较厚的 `SequenceAwaitable + SequenceStep` 业务类
- HTTP/2 / WebSocket / SSL 路径里仍有 `CustomAwaitable` 或 HTTP 私有执行骨架，例如 `InternalPipelineAwaitable`

这些实现已经能在新 kernel 下编译运行，但还没有对齐到最终推荐写法。

## 2. 设计目标

本轮设计的目标不是机械替换 API，而是统一内部抽象边界：

- 协议状态与解析逻辑放进 `Flow` 或 `Machine`
- 执行逻辑只由 kernel/ssl 的 builder 或 state-machine core 承担
- HTTP 模块不再维护自己的 awaitable 执行器
- 用户扩展新协议时，不需要模仿 `CustomAwaitable` 样板

同时保留现有高层入口的可用性：

- `HttpClient`
- `HttpSession`
- `HttpReader`
- `HttpWriter`
- `WsClient` / `WsSession`
- `H2cClient` / `Http2Conn`

高层类型可以继续保留方法名和语义，但内部实现要收敛到 builder/state-machine。

## 3. 总体架构

### 3.1 线性流：统一使用 `AwaitableBuilder`

适用场景：

- HTTP/1.x request/response 读取
- 常规 request/response 写入
- 单次 WebSocket frame 读取或写入
- 单次 chunk 读取或写入

统一模式：

1. 会话对象持有 socket、ring buffer、request/response、配置和 `Flow`
2. 某个方法里现建现用 builder
3. 通过 `recv(...).parse(...).send(...).finish(...).build()` 生成 awaitable
4. `parse handler` 只负责消费 buffer 并返回 `ParseStatus`

这意味着不推荐把 `AwaitableBuilder` 做成类成员；builder 是一次性的装配器，状态应放在 `Flow` 或会话对象里。

### 3.2 复杂流：统一使用 `fromStateMachine(...)`

适用场景：

- SSL 路径下的 HTTP 读写
- HTTP/2 连接级帧收发、preface、shutdown、流状态切换
- WebSocket message 聚合、控制帧穿插、关闭握手

统一模式：

1. 定义 `Machine::result_type`
2. 用 machine 显式表达 `want_read` / `want_write` / local action / completion
3. 通过 `AwaitableBuilder<Result>::fromStateMachine(controller, machine).build()` 暴露 awaitable

这类路径不再通过 `CustomAwaitable`、HTTP 私有队列执行器、或“兼容旧 awaitable 的壳”来推进状态。

## 4. 组件分层

### 4.1 Flow / Machine

职责：

- 保存跨步骤状态
- 维护解析中间态、错误态、累计字节数、结果对象
- 提供 `onRecv` / `onParse` / `onSend` / `onFinish`
- 或提供 `MachineAction` 风格的状态机动作

非职责：

- 不直接继承 awaitable
- 不负责调度 IO
- 不向 reactor 暴露内部上下文对象

### 4.2 Builder Facade

职责：

- 把 socket、flow、buffer 和 handler 串起来
- 为上层保留易读、稳定的调用入口
- 返回内核自带的 machine-backed awaitable

示意：

```cpp
auto HttpClient::recvResponseAwaitable() {
    return AwaitableBuilder<Result, 4, ResponseFlow>(m_socket.controller(), m_response_flow)
        .recv<&ResponseFlow::onRecv>(m_response_flow.buffer.data(), m_response_flow.buffer.size())
        .parse<&ResponseFlow::onParse>()
        .finish<&ResponseFlow::onFinish>()
        .build();
}
```

### 4.3 薄兼容层

若为了源码兼容必须保留旧命名类型，例如 `GetResponseAwaitableImpl`，也只允许保留为：

- builder/state-machine 的薄封装
- `using` 别名
- 单一返回值适配

不允许再在这些类型里堆放协议状态、`IOContext` 成员、或 awaitable 调度骨架。

## 5. 具体迁移边界

### 5.1 HTTP/1.x

收敛对象：

- `galay-http/kernel/http/HttpReader.h`
- `galay-http/kernel/http/HttpWriter.h`
- `galay-http/kernel/http/HttpSession.h`

方向：

- `TcpSocket` 路径由当前厚 `SequenceAwaitable` 业务类继续收敛到 builder facade
- SSL 路径删除 `CustomAwaitable + SslRecvCompatAwaitable` 套娃，改为直接使用 SSL state-machine/builder
- request/response/chunk 读写的 buffer window、错误映射、`ParseStatus` 判定抽成共享 helper

### 5.2 WebSocket

收敛对象：

- `galay-http/kernel/websocket/WsReader.h`
- `galay-http/kernel/websocket/WsWriter.h`
- `galay-http/kernel/websocket/WsSession.h`
- `galay-http/kernel/websocket/WsClient.h`

方向：

- 单帧读写优先 builder
- message 聚合、close frame、ping/pong 穿插用 state machine
- 彻底移除 HTTP 私有 pipeline 执行器

### 5.3 HTTP/2

收敛对象：

- `galay-http/kernel/http2/Http2Conn.h`
- `galay-http/kernel/http2/H2cClient.h`
- `galay-http/kernel/http2/Http2ConnectionCore.h`
- `galay-http/kernel/http2/Http2ConnectionCore.cc`
- `galay-http/kernel/http2/Http2Server.h`
- `galay-http/kernel/http2/Http2StreamManager.h`

方向：

- 连接级持续收发与帧解析显式表达为 machine
- 单次线性帧发送或局部本地处理仍可用 builder/local step
- 删除 `InternalPipelineAwaitable`

## 6. 对外 API 策略

本轮属于大版本升级，按 breaking change 处理，但尽量保住“高层入口的方法名和使用意图”。

策略如下：

- 尽量保留 `HttpClient` / `HttpSession` / `WsSession` / `H2cClient` 的方法名
- 新文档与示例统一改用 builder/state-machine 表达
- 不再新增新的“协议专用 awaitable 类型”
- 旧 awaitable 名称若保留，只允许做薄 facade，不承载执行细节

## 7. 风险与应对

### 7.1 模板面回归风险

风险：

- 头文件模板改造后，用户现有 `auto aw = session.xxxAwaitable()` 的编译面可能变化

应对：

- 增加 surface compile tests，锁住公开头文件与关键返回表面
- 对常见入口优先保持方法名不变

### 7.2 SSL / HTTP2 行为回归风险

风险：

- `want_read` / `want_write` 与 shutdown/preface 逻辑容易在重构时出现细小时序变化

应对：

- 用独立 state-machine tests 覆盖握手、读写切换、关闭路径
- 先锁失败测试，再做最小重构

### 7.3 性能退化风险

风险：

- 过度抽象可能增加额外拷贝或更多本地循环开销

应对：

- 保留 ring buffer / iovec window 设计
- 用 benchmark 对比改造前后吞吐、延迟与单连接循环开销

## 8. 验证要求

### 8.1 编译面

- `T57-awaitable_builder_parser`
- `T58-ws_pipeline_surface`
- 新增或扩展 HTTP/2 / SSL state-machine surface tests

### 8.2 回归测试

至少覆盖：

- HTTP client/server
- HTTPS client/server
- WebSocket client/server
- H2C / H2 client/server
- timeout / router / chunk / proxy 等易受 reader/writer 改动影响的场景

### 8.3 压测与性能对比

至少输出：

- HTTP/1.x client/server benchmark
- WebSocket server benchmark
- WSS server benchmark
- H2C client benchmark

结果写入仓库内文档，并归档到 rollout 结果目录。

## 9. 发布要求

`galay-http` 以及所有跟随本轮发生改动的 `galay-*` 仓库，最终统一满足：

- 全量回归通过
- 压测结果可对比
- 文档更新完成
- 中文提交
- 新大版本 tag
- release notes 清晰描述 builder/state-machine 收敛与旧 awaitable 退场

## 10. 结论

本轮 `galay-http` 不做“继续兼容旧 awaitable 风格”的保守修补，而是把 Awaitable 体系一次性收敛到：

- 线性流：`AwaitableBuilder`
- 复杂流：`AwaitableBuilder::fromStateMachine(...)`

这样既对齐 `galay-kernel` / `galay-ssl` 的最终推荐模型，也能显著降低 HTTP 模块继续维护私有 awaitable 执行器的复杂度。
