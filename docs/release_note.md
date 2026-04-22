# Release Note

按时间顺序追加版本记录，避免覆盖历史发布说明。

## v2.1.0 - 2026-04-22

- 版本级别：中版本（minor）
- Git 提交消息：`chore: 发布 v2.1.0`
- Git Tag：`v2.1.0`
- 自述摘要：
  - 新增 `HttpSession::sendSerializedRequest(std::string)`，允许上层直接发送完整 `HTTP/1.x` 请求报文，同时复用现有超时控制、收包与响应解析状态机。
  - 为 `HttpSession::post(...)` 增加右值请求体重载，减少热点请求路径的一次 body 拷贝，并补齐 `T78-http_session_serialized_request` 回归测试。
  - 将源码构建、测试与导出包配置中的 `galay-kernel` 依赖基线统一提升到 `3.4.5`，保持与最新内核发布一致。
