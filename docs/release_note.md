# Release Note

按时间顺序追加版本记录，避免覆盖历史发布说明。

## v2.1.2 - 2026-04-23

- 版本级别：小版本（patch）
- Git 提交消息：`chore: 默认关闭测试构建`
- Git Tag：`v2.1.2`
- 自述摘要：
  - 将 `BUILD_TESTING` 固化为 `galay-http` 的测试主开关，避免配置阶段因 `CTest` 默认值导致测试树被隐式开启。
  - 在未显式设置测试开关时默认强制关闭测试构建，保持源码仓库默认路径与其他 `galay-*` 组件一致。
  - 保留 `BUILD_TESTS` 兼容别名，已有脚本仍可通过旧参数显式恢复测试目标。

## v2.1.1 - 2026-04-23

- 版本级别：小版本（patch）
- Git 提交消息：`chore: 发布 v2.1.1`
- Git Tag：`v2.1.1`
- 自述摘要：
  - 将源码仓库里的包配置模板统一为小写 kebab-case 命名，并同步修正 `configure_package_config_file(...)` 的模板输入路径。
  - 保持安装导出的 `GalayHttpConfig.cmake`、版本文件与 `GalayHttp` 包名兼容不变，避免影响现有 `find_package(GalayHttp)` 消费者。
  - 删除 `docker/galay-sdk/` 旧目录下的构建、校验和远程运行脚本，清理历史遗留的 SDK 容器工作流。

## v2.1.0 - 2026-04-22

- 版本级别：中版本（minor）
- Git 提交消息：`chore: 发布 v2.1.0`
- Git Tag：`v2.1.0`
- 自述摘要：
  - 新增 `HttpSession::sendSerializedRequest(std::string)`，允许上层直接发送完整 `HTTP/1.x` 请求报文，同时复用现有超时控制、收包与响应解析状态机。
  - 为 `HttpSession::post(...)` 增加右值请求体重载，减少热点请求路径的一次 body 拷贝，并补齐 `T78-http_session_serialized_request` 回归测试。
  - 将源码构建、测试与导出包配置中的 `galay-kernel` 依赖基线统一提升到 `3.4.5`，保持与最新内核发布一致。

## v2.1.3 - 2026-04-26

- 版本级别：小版本（patch）
- Git 提交消息：`test: 修复 iouring 合约测试构建`
- Git Tag：`v2.1.3`
- 自述摘要：
  - 修复 `iouring` 合约测试在多处 HTTP、WebSocket、HTTP/2 与 TLS 相关用例中的构建问题。
  - 为受影响测试目标补齐缺失头文件与必要依赖声明，恢复 `iouring` 测试矩阵下的可编译性。

## v3.0.0 - 2026-04-29

- 版本级别：大版本（major）
- Git 提交消息：`refactor: 统一源码文件命名规范`
- Git Tag：`v3.0.0`
- 自述摘要：
  - 将源码、头文件、测试、示例与 benchmark 文件统一重命名为 lower_snake_case，编号前缀同步改为小写下划线形式。
  - 同步更新 CMake/Bazel 构建描述、模块入口、README/docs、脚本和所有项目内 include 路径引用。
  - 移除项目内相对 include，统一使用基于公开 include 根或模块根的非相对路径。
