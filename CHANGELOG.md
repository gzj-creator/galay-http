# CHANGELOG

维护说明：
- 未打 tag 的改动先写入 `## [Unreleased]`。
- 需要发版时，从 `Unreleased` 或“自上次 tag 以来”的累计变更整理出新的版本节。
- 版本号遵循 `major/minor/patch` 规则：大改动升主版本，新功能升次版本，修复与非破坏性维护升修订版本。
- 推荐标题格式为 `## [vX.Y.Z] - YYYY-MM-DD`，正文按 `Added` / `Changed` / `Fixed` / `Docs` / `Chore` 归纳。

## [Unreleased]

## [v2.1.3] - 2026-04-26

### Fixed
- 修复 `iouring` 合约测试在多处 HTTP / WS / H2 用例中的构建问题，补齐缺失头文件与相关测试依赖，恢复测试目标可编译性。

## [v2.1.2] - 2026-04-23

### Changed
- 将测试构建入口统一为 `BUILD_TESTING`，并在用户未显式开启时默认关闭测试构建。
- 保留 `BUILD_TESTS` 兼容别名，已有脚本仍可显式恢复测试目标。

## [v2.1.1] - 2026-04-23

### Changed
- 将源码仓库中的包配置模板重命名为统一的小写 kebab-case `galay-http-config.cmake.in`，与其他 `galay-*` 项目的模板命名保持一致。
- 同步更新 `configure_package_config_file(...)` 的模板路径，继续保留安装导出的 `GalayHttpConfig.cmake` / `GalayHttpConfigVersion.cmake` 与 `GalayHttp` 包名兼容。

### Chore
- 删除 `docker/galay-sdk/` 下的构建、校验与远程运行脚本，清理已经不再维护的 HTTP SDK 镜像工作流目录。

## [v2.1.0] - 2026-04-22

### Added
- 新增 `HttpSession::sendSerializedRequest(std::string)` 高级入口，允许直接发送预序列化 `HTTP/1.x` 报文，并复用会话层的超时控制与响应解析状态机。
- 新增 `HttpSession::post(..., std::string&& body, ...)` 右值重载，减少热点路径下一次请求体拷贝。

### Changed
- 将源码构建与导出配置中的 `galay-kernel` 依赖基线提升到 `3.4.5`，与最新内核发布版本保持一致。

### Docs
- 补充 `HttpSession` 高级请求入口的 API 语义说明，并新增 `T78-http_session_serialized_request` 回归测试覆盖序列化请求发送路径。
