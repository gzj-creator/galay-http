# CHANGELOG

维护说明：
- 未打 tag 的改动先写入 `## [Unreleased]`。
- 需要发版时，从 `Unreleased` 或“自上次 tag 以来”的累计变更整理出新的版本节。
- 版本号遵循 `major/minor/patch` 规则：大改动升主版本，新功能升次版本，修复与非破坏性维护升修订版本。
- 推荐标题格式为 `## [vX.Y.Z] - YYYY-MM-DD`，正文按 `Added` / `Changed` / `Fixed` / `Docs` / `Chore` 归纳。

## [Unreleased]

## [v2.1.0] - 2026-04-22

### Added
- 新增 `HttpSession::sendSerializedRequest(std::string)` 高级入口，允许直接发送预序列化 `HTTP/1.x` 报文，并复用会话层的超时控制与响应解析状态机。
- 新增 `HttpSession::post(..., std::string&& body, ...)` 右值重载，减少热点路径下一次请求体拷贝。

### Changed
- 将源码构建与导出配置中的 `galay-kernel` 依赖基线提升到 `3.4.5`，与最新内核发布版本保持一致。

### Docs
- 补充 `HttpSession` 高级请求入口的 API 语义说明，并新增 `T78-http_session_serialized_request` 回归测试覆盖序列化请求发送路径。
