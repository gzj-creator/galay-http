# HTTPS/SSL 支持待办列表

## 目标
通过模板化 TcpSocket，支持 TcpSocket 和 SslSocket，让用户可以选择 HTTP 或 HTTPS。

## CMake 配置 (cmake/options.cmake)
- [x] 创建 cmake/options.cmake 文件
- [x] 添加 GALAY_HTTP_ENABLE_SSL 选项（默认 OFF）
- [x] 当 SSL 启用时，查找并链接 galay-socket 库
- [x] 添加 GALAY_HTTP_SSL_ENABLED 编译宏

## 协议层模板化 (protoc/)
- [x] 无需修改，协议层与传输层无关

## 内核层模板化 (kernel/)

### HttpConn 模板化
- [x] 将 HttpConn 改为模板类 `template<typename SocketType>`
- [x] 支持 TcpSocket 和 SslSocket
- [x] 提供类型别名：
  - `using HttpConn = HttpConnImpl<TcpSocket>`
  - `using HttpsConn = HttpConnImpl<SslSocket>` (仅 SSL 启用时)

### HttpReader 模板化
- [x] 将 HttpReader 改为模板类
- [x] GetRequestAwaitable 模板化
- [x] GetResponseAwaitable 模板化
- [x] GetChunkAwaitable 模板化

### HttpWriter 模板化
- [x] 将 HttpWriter 改为模板类
- [x] SendResponseAwaitable 模板化

### HttpServer 模板化
- [x] 将 HttpServer 改为模板类
- [x] 提供类型别名：
  - `using HttpServer = HttpServerImpl<TcpSocket>`
  - `using HttpsServer = HttpServerImpl<SslSocket>` (仅 SSL 启用时)
- [ ] HttpsServer 需要额外的 SSL 配置（证书、私钥路径）- TODO: 完善 SSL 上下文初始化

### HttpClient 模板化
- [x] 将 HttpClient 改为模板类
- [x] 提供类型别名：
  - `using HttpClient = HttpClientImpl<TcpSocket>`
  - `using HttpsClient = HttpClientImpl<SslSocket>` (仅 SSL 启用时)

### WebSocket 模板化
- [x] WsConn 模板化
- [x] WsReader 模板化
- [x] WsWriter 模板化
- [ ] WsClient 模板化 - 保留原有实现，后续优化

## 头文件组织
- [ ] 创建 galay-http/ssl/SslConfig.h（SSL 配置结构）- 后续实现
- [x] 条件包含 SSL 相关头文件

## CMakeLists.txt 更新
- [x] 在根 CMakeLists.txt 中 include(cmake/options.cmake)
- [x] galay-http/CMakeLists.txt 中条件链接 galay-socket
- [x] 移除已模板化的 .cc 文件

## 测试
- [ ] 添加 HTTP 测试
- [ ] 添加 HTTPS 测试（仅 SSL 启用时编译）

## 文档
- [ ] 更新 README 说明 SSL 选项
- [ ] 添加 HTTPS 使用示例

## 实现顺序
1. [x] 创建 cmake/options.cmake
2. [x] 模板化 HttpConn
3. [x] 模板化 HttpReader/HttpWriter
4. [x] 模板化 HttpServer/HttpClient
5. [x] 模板化 WebSocket 相关类（部分完成）
6. [x] 更新 CMakeLists.txt
7. [ ] 编写测试
8. [ ] 更新文档
