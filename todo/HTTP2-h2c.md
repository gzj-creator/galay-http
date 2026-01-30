# HTTP/2 h2c 实现待办列表

## 协议层 (protoc/http2)
- [false] 实现 HTTP/2 帧基础结构 (Http2Frame.h/cc)
- [false] 实现 HTTP/2 帧类型定义和解析
- [false] 实现 HPACK 头部压缩/解压缩 (Http2Hpack.h/cc)
- [false] 实现 HTTP/2 流管理 (Http2Stream.h/cc)
- [false] 实现 HTTP/2 错误码定义 (Http2Error.h)
- [false] 实现 HTTP/2 设置帧处理 (Http2Settings.h/cc)

## 内核层 (kernel/http2)
- [false] 实现 HTTP/2 连接类 (Http2Conn.h/cc)
- [false] 实现 HTTP/2 读取器 (Http2Reader.h/cc)
- [false] 实现 HTTP/2 写入器 (Http2Writer.h/cc)
- [false] 实现 HTTP/2 服务器 (Http2Server.h/cc)
- [false] 实现 HTTP/2 客户端 (Http2Client.h/cc)

## h2c 升级支持
- [false] 实现 HTTP/1.1 到 HTTP/2 的 h2c 升级机制
- [false] 实现 HTTP/2 连接前言处理

## 测试
- [false] 编写 HTTP/2 帧解析测试
- [false] 编写 HPACK 编解码测试
- [false] 编写 HTTP/2 服务器测试
- [false] 编写 HTTP/2 客户端测试

## 文档和脚本
- [false] 更新 CMakeLists.txt
- [false] 编写测试文档
