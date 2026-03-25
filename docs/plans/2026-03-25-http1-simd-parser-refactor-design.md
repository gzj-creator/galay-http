# HTTP/1.x SIMD 解析重构设计

## 背景

当前 HTTP/1.x 解析已经采用增量状态机，并支持基于 `iovec` 的输入，但热点路径仍主要集中在以下几类逐字节扫描上：

- request line / status line 中查找空格、`\r`、`\n`
- header key 中查找 `:`
- header value 中查找 `\r`
- ServerSide 模式下 header key 的逐字节 ASCII 小写归一化

这些逻辑目前分散在 `HttpRequestHeader::fromIOVec` 与 `HttpResponseHeader::fromIOVec` 中，重复较多，也限制了 SIMD 优化的落点。

## 目标

- 为 HTTP/1.x 头解析引入统一的 SIMD 扫描层
- 保持现有外部 API、错误码和增量解析语义不变
- 让 request/response 头解析复用同一套热点扫描能力
- 为后续 chunk line 等协议扫描复用预留内部接口

## 非目标

- 不改动 HTTP/2 / HPACK 主解析流程
- 不改动对外公开 API
- 不把多段 `iovec` 复制为连续缓冲后再解析
- 不在本轮合并 `HttpChunk.cc` 的现有 SIMD 逻辑

## 方案对比

### 方案一：统一 SIMD 扫描器 + 头解析状态机重构

在 `protoc/http` 内新增内部扫描工具，统一提供分隔符查找和 ASCII lowercase copy 能力，由 request/response 头解析复用。

- 优点：热点集中，收益最大，后续复用性最好
- 缺点：改动面较大，需要严格回归增量解析边界

### 方案二：局部热点替换

保留现有状态机形状，仅把若干 `while` 扫描替换为 SIMD helper。

- 优点：风险较低，改动快
- 缺点：收益受限，重复逻辑仍然存在

### 方案三：完整 header block 预扫描 + 二阶段解析

先整体扫描 header block，再二次解析结构。

- 优点：理论上峰值吞吐更高
- 缺点：与当前流式 `iovec` 模型不匹配，复杂度和风险都高

最终选择方案一。

## 架构设计

新增内部文件，例如：

- `galay-http/protoc/http/HttpSimdScan.h`

该层仅负责：

- 在连续内存中快速定位 ` `、`:`、`\r`、`\n`
- 批量执行 ASCII uppercase -> lowercase copy
- 在平台不支持 SIMD 或数据块过小时回退到标量实现

平台支持范围：

- x86: SSE2
- ARM: NEON
- 其他平台：标量回退

解析层保持现有职责：

- `HttpRequestHeader` / `HttpResponseHeader` 继续持有状态机状态
- 继续负责协议合法性判断与错误码返回
- 继续维护增量解析所需的中间字符串和已消费字节语义

## 数据流设计

本次不引入“跨 `iovec` 拼成连续缓冲”的中间层，而是采用“连续块扫描 + 状态机拼接”的方式：

1. `fromIOVec` 继续按顺序遍历 `iovecs`
2. 每次拿到一个连续块 `[ptr, len]` 后调用 `HttpSimdScan`
3. `HttpSimdScan` 返回块内结构位置信息，不持有跨块状态
4. request/response 状态机根据扫描结果推进解析阶段

状态推进规则：

- `Method` / `Uri` / `Version` / `Code` / `Status`
  - 统一使用“查找空格或行终止符”扫描
- `HeaderKey`
  - 使用“查找 `:` 或非法换行”扫描
  - ServerSide 模式下直接 lower-copy 到目标字符串
  - 遇到 `:` 后立即进行 common header 匹配
- `HeaderValue`
  - 使用“查找 `\r`”扫描
  - 普通字符整段追加，不再逐字节 `push_back`
- `CRLF` / `HeaderEnd`
  - 继续保留显式、严格的状态校验

跨 `iovec` 边界策略：

- SIMD 仅在单个连续块内工作
- 若分隔符跨块，则由状态机在下一块继续解析
- `\r\n` 等双字节结构继续由状态机校验完整性

## 错误处理与兼容性

- `HttpErrorCode` 保持不变
- `fromIOVec` 返回的 `consumed` 语义保持不变
- header 完整、部分完整、失败时的行为保持不变
- SIMD 仅负责“找到哪里停”，不负责“协议是否合法”
- 小块数据直接回退标量路径，避免 SIMD 启动成本吞掉收益

扫描器接口要求与标量实现语义等价：

- 找到返回块内偏移
- 找不到返回 `npos`
- lowercase copy 仅转换 ASCII `A-Z`

## 测试与验证

### 功能回归

扩展 [test/T1-http_parser.cc](/Users/gongzhijie/Desktop/projects/git/galay-http/test/T1-http_parser.cc)，覆盖：

- request/response line 跨 `iovec` 边界
- `:` 跨块
- `\r\n` 跨块
- 超长但合法的 header key/value
- 非法换行
- key 超长错误

### 扫描器单测

新增独立测试覆盖：

- ` ` / `:` / `\r` / `\n` / `CR-or-LF` 的定位
- 长度边界：0、1、15、16、17、31、32
- 命中在块首、块尾、无命中
- lowercase copy 的 ASCII 行为

### Benchmark

新增仓库内可执行的轻量 benchmark，用于对比标量路径和 SIMD 路径的吞吐，至少覆盖：

- 常见短 header 组合
- 较长 header key/value 场景

## 风险与控制

- 主要风险是增量解析边界回归，而不是 SIMD 指令本身
- request/response 两套解析收敛后，需要避免在共享 helper 上引入行为偏差
- 本轮仅收敛头解析热点，不扩大到 HTTP/2 或 chunk 逻辑，控制改动范围

## 预期收益

- 降低 header 解析中的逐字节分支开销
- 减少 request/response 解析中的重复扫描逻辑
- 为后续 chunk / 其他文本协议扫描复用内部 SIMD helper 打基础
