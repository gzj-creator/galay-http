# T5-HTTP 服务器测试

## 测试概述

本文档记录 `HttpServer` 类的功能测试结果。测试验证了完整的 HTTP 服务器实现，包括请求处理、路由分发、响应生成和连接管理。

## 测试目标

验证 `HttpServer` 的完整功能：
- 服务器启动和配置
- HTTP 请求接收和解析
- 路由处理和响应生成
- 多种内容类型支持
- 404 错误处理
- 并发连接处理

## 测试场景

### 1. 服务器配置和启动

#### 1.1 服务器配置
- **配置项**：
  ```cpp
  HttpServerConfig config;
  config.host = "127.0.0.1";
  config.port = 8080;
  config.backlog = 128;
  ```
- **验证点**：
  - 配置正确应用
  - 服务器成功启动

#### 1.2 请求处理器
- **处理器类型**：协程函数 `handleRequest(HttpConn conn)`
- **功能**：
  - 接收 `HttpConn` 连接对象
  - 读取请求
  - 生成响应
  - 关闭连接

### 2. 路由处理测试

#### 2.1 首页路由（/ 或 /index.html）
- **请求**：`GET /` 或 `GET /index.html`
- **响应**：
  - 状态码：200 OK
  - Content-Type：text/html; charset=utf-8
  - 内容：HTML 首页，包含导航链接
- **验证点**：
  - HTML 格式正确
  - 包含链接到其他页面
  - 字符编码正确

#### 2.2 Hello 页面（/hello）
- **请求**：`GET /hello`
- **响应**：
  - 状态码：200 OK
  - Content-Type：text/html; charset=utf-8
  - 内容：Hello 页面，包含返回首页链接
- **验证点**：
  - 页面内容正确
  - 导航链接有效

#### 2.3 Test 页面（/test）
- **请求**：`GET /test`
- **响应**：
  - 状态码：200 OK
  - Content-Type：text/html; charset=utf-8
  - 内容：测试页面
- **验证点**：
  - 页面渲染正确

#### 2.4 API 接口（/api/info）
- **请求**：`GET /api/info`
- **响应**：
  - 状态码：200 OK
  - Content-Type：application/json
  - 内容：JSON 格式的服务器信息
  ```json
  {
      "server": "galay-http",
      "version": "1.0.0",
      "status": "running",
      "timestamp": "1738137600"
  }
  ```
- **验证点**：
  - JSON 格式正确
  - Content-Type 正确设置
  - 时间戳有效

#### 2.5 404 错误处理
- **请求**：`GET /nonexistent`
- **响应**：
  - 状态码：404 Not Found
  - Content-Type：text/html; charset=utf-8
  - 内容：404 错误页面
- **验证点**：
  - 状态码正确
  - 错误页面友好
  - 包含返回首页链接

### 3. HTTP 协议支持

#### 3.1 请求方法
- **支持的方法**：GET、POST（可扩展）
- **验证点**：
  - 正确识别请求方法
  - 方法记录到日志

#### 3.2 HTTP 版本
- **支持版本**：HTTP/1.1
- **响应版本**：HTTP/1.1
- **验证点**：
  - 版本协商正确

#### 3.3 响应头
- **标准头部**：
  - `Content-Type`：根据内容类型设置
  - `Content-Length`：精确的 body 长度
  - `Server`：`GALAY_SERVER` 标识
- **验证点**：
  - 所有必需头部存在
  - 头部值正确

### 4. 连接管理

#### 4.1 连接接受
- **测试内容**：接受客户端连接
- **验证点**：
  - 记录客户端信息
  - 连接计数正确

#### 4.2 请求计数
- **测试内容**：使用 `g_request_count` 统计请求
- **验证点**：
  - 计数递增正确
  - 线程安全（atomic）

#### 4.3 连接关闭
- **测试内容**：请求处理完成后关闭连接
- **验证点**：
  - 调用 `conn.close()` 成功
  - 资源正确释放

### 5. 错误处理

#### 5.1 解析错误
- **测试内容**：处理请求解析错误
- **验证点**：
  - 检测到解析错误
  - 记录错误日志
  - 关闭连接

#### 5.2 连接断开
- **测试内容**：处理客户端主动断开
- **验证点**：
  - 检测到 `kConnectionClose`
  - 优雅处理断开

## 测试用例列表

| 编号 | 路由 | 方法 | 状态码 | Content-Type | 结果 |
|------|------|------|--------|--------------|------|
| 1 | / | GET | 200 | text/html | ✓ |
| 2 | /index.html | GET | 200 | text/html | ✓ |
| 3 | /hello | GET | 200 | text/html | ✓ |
| 4 | /test | GET | 200 | text/html | ✓ |
| 5 | /api/info | GET | 200 | application/json | ✓ |
| 6 | /nonexistent | GET | 404 | text/html | ✓ |

## 测试代码位置

- **文件路径**：`/Users/gongzhijie/Desktop/projects/git/galay-http/test/T5-HttpServer.cc`
- **代码行数**：208 行
- **主要函数**：
  - `handleRequest(HttpConn conn)`：请求处理协程
  - `main()`：服务器启动入口

## 核心 API 说明

### HttpServer 配置

```cpp
struct HttpServerConfig {
    std::string host;    // 监听地址
    uint16_t port;       // 监听端口
    int backlog;         // 监听队列长度
};
```

### HttpServer 类

```cpp
class HttpServer {
public:
    HttpServer(const HttpServerConfig& config);

    // 启动服务器（阻塞）
    void start(std::function<Coroutine(HttpConn)> handler);

    // 停止服务器
    void stop();
};
```

### HttpConn 类

```cpp
class HttpConn {
public:
    // 获取 Reader
    HttpReader& getReader();

    // 获取 Writer
    HttpWriter& getWriter();

    // 关闭连接
    Awaitable<void> close();
};
```

## 运行测试

### 编译测试

```bash
cd build
cmake ..
make T5-HttpServer
```

### 运行服务器

```bash
./test/T5-HttpServer
```

### 预期输出

```
========================================
HTTP Server Test
========================================

========================================
HTTP Server is running on http://127.0.0.1:8080
========================================
Available endpoints:
  - http://127.0.0.1:8080/
  - http://127.0.0.1:8080/hello
  - http://127.0.0.1:8080/test
  - http://127.0.0.1:8080/api/info
========================================
Press Ctrl+C to stop the server
========================================

Request #1 received: 0 /
Response sent: complete

Request #2 received: 0 /hello
Response sent: complete
```

### 使用浏览器测试

打开浏览器访问：
- http://127.0.0.1:8080/
- http://127.0.0.1:8080/hello
- http://127.0.0.1:8080/test
- http://127.0.0.1:8080/api/info

### 使用 curl 测试

```bash
# 测试首页
curl http://127.0.0.1:8080/

# 测试 API
curl http://127.0.0.1:8080/api/info

# 测试 404
curl http://127.0.0.1:8080/nonexistent
```

## 测试结论

### 功能验证

✅ **服务器启动正常**：配置正确应用，监听成功
✅ **路由处理完善**：支持多个路由，正确分发请求
✅ **内容类型支持**：HTML 和 JSON 都能正确处理
✅ **错误处理健壮**：404 错误友好处理
✅ **并发支持**：每个连接独立协程处理
✅ **连接管理正确**：优雅处理连接建立和关闭

### 架构特点

- **协程驱动**：基于 C++20 协程的异步处理
- **事件驱动**：支持 Kqueue、Epoll、IOUring
- **高并发**：每个连接独立协程，无阻塞
- **模块化**：HttpConn 封装连接，HttpReader/Writer 分离职责

### 性能特点

- **非阻塞 I/O**：所有 I/O 操作异步执行
- **零拷贝**：RingBuffer + iovec 减少内存拷贝
- **低延迟**：协程切换开销小
- **高吞吐**：事件驱动模型支持大量并发

### 路由设计

当前实现使用简单的 if-else 路由：

```cpp
if (uri == "/" || uri == "/index.html") {
    // 首页
} else if (uri == "/hello") {
    // Hello 页面
} else if (uri == "/api/info") {
    // API 接口
} else {
    // 404
}
```

**可扩展为**：
- 路由表（map）
- 正则表达式匹配
- 路由树（Trie）
- 中间件链

### 适用场景

1. **Web 服务器**：静态网站托管
2. **API 服务**：RESTful API 后端
3. **微服务**：轻量级 HTTP 服务
4. **开发测试**：本地开发服务器

### 扩展建议

1. **路由系统**：实现路由表和参数提取
2. **中间件**：添加日志、认证、CORS 等中间件
3. **静态文件**：支持静态文件服务
4. **模板引擎**：集成 HTML 模板引擎
5. **WebSocket**：添加 WebSocket 支持
6. **HTTPS**：集成 TLS/SSL 支持

---

**测试日期**：2026-01-29
**测试人员**：galay-http 开发团队
**文档版本**：v1.0
