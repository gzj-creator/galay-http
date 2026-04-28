# CHANGELOG

维护说明：
- 未打 tag 的改动先写入 `

## [Unreleased]

## [v3.0.0] - 2026-04-29

### Changed
- 统一源码、头文件、测试、示例与 benchmark 文件命名为 `lower_snake_case`，编号前缀同步使用 `t<number>_`、`e<number>_` 与 `b<number>_` 风格。
- 同步更新构建脚本、模块入口、示例、测试、文档与脚本中的文件路径引用。
- 将项目内头文件包含调整为基于公开 include 根或模块根的非相对路径。

### Release
- 按大版本发布要求提升版本到 `v3.0.0`。

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
