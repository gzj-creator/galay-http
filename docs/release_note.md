# Release Note

按时间顺序追加版本记录，避免覆盖历史发布说明。

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
