# T8-HTTP 方法测试

## 测试概述

本文档记录 `HttpClient` 所有 HTTP 方法的测试结果。测试覆盖了 HTTP/1.1 规范定义的 9 种标准方法，验证客户端对各种 HTTP 方法的支持。

## 测试目标

验证 HttpClient 对所有 HTTP 方法的支持：
- GET：获取资源
- POST：创建资源
- PUT：更新资源
- DELETE：删除资源
- HEAD：获取资源元数据
- OPTIONS：查询支持的方法
- PATCH：部分更新资源
- TRACE：诊断追踪
- CONNECT：建立隧道

## HTTP 方法说明

### 标准 HTTP 方法

| 方法 | 用途 | 是否有 Body | 幂等性 | 安全性 |
|------|------|------------|--------|--------|
| GET | 获取资源 | ❌ | ✅ | ✅ |
| POST | 创建资源 | ✅ | ❌ | ❌ |
| PUT | 更新资源 | ✅ | ✅ | ❌ |
| DELETE | 删除资源 | ❌ | ✅ | ❌ |
| HEAD | 获取元数据 | ❌ | ✅ | ✅ |
| OPTIONS | 查询方法 | ❌ | ✅ | ✅ |
| PATCH | 部分更新 | ✅ | ❌ | ❌ |
| TRACE | 诊断追踪 | ❌ | ✅ | ✅ |
| CONNECT | 建立隧道 | ❌ | ❌ | ❌ |

## 测试场景

### 1. GET 方法测试

#### 1.1 基本 GET 请求
- **测试内容**：`GET /api/data`
- **请求特点**：
  - 无请求体
  - 用于获取资源
- **验证点**：
  - 请求发送成功
  - 响应状态码 200
  - 响应体包含数据
  - Body 长度正确

#### 1.2 API 调用
```cpp
auto result = co_await client.get("/api/data");
```

### 2. POST 方法测试

#### 2.1 JSON POST 请求
- **测试内容**：`POST /api/data`
- **请求体**：
  ```json
  {"name": "test", "value": 123}
  ```
- **Content-Type**：application/json
- **验证点**：
  - 请求体正确发送
  - Content-Type 正确设置
  - Content-Length 正确计算
  - 响应状态码 200

#### 2.2 API 调用
```cpp
std::string body = R"({"name": "test", "value": 123})";
auto result = co_await client.post("/api/data", body, "application/json");
```

### 3. PUT 方法测试

#### 3.1 更新资源
- **测试内容**：`PUT /api/data/1`
- **请求体**：
  ```json
  {"name": "updated", "value": 456}
  ```
- **Content-Type**：application/json
- **验证点**：
  - PUT 请求正确构造
  - 资源 ID 在 URI 中
  - 请求体完整发送
  - 响应状态码 200

#### 3.2 API 调用
```cpp
std::string body = R"({"name": "updated", "value": 456})";
auto result = co_await client.put("/api/data/1", body, "application/json");
```

### 4. DELETE 方法测试

#### 4.1 删除资源
- **测试内容**：`DELETE /api/data/1`
- **请求特点**：
  - 无请求体
  - 资源 ID 在 URI 中
- **验证点**：
  - DELETE 请求发送成功
  - 响应状态码 200 或 204
  - 响应体可能为空

#### 4.2 API 调用
```cpp
auto result = co_await client.del("/api/data/1");
```

### 5. HEAD 方法测试

#### 5.1 获取资源元数据
- **测试内容**：`HEAD /api/data`
- **请求特点**：
  - 无请求体
  - 响应无 body（只有头部）
- **验证点**：
  - HEAD 请求发送成功
  - 响应状态码 200
  - 响应体长度为 0
  - 响应头包含 Content-Length

#### 5.2 API 调用
```cpp
auto result = co_await client.head("/api/data");
```

### 6. OPTIONS 方法测试

#### 6.1 查询支持的方法
- **测试内容**：`OPTIONS /api/data`
- **请求特点**：
  - 无请求体
  - 用于 CORS 预检
- **验证点**：
  - OPTIONS 请求发送成功
  - 响应状态码 200 或 204
  - 响应头包含 Allow 字段
  - Allow 字段列出支持的方法

#### 6.2 API 调用
```cpp
auto result = co_await client.options("/api/data");
```

#### 6.3 Allow 头部示例
```
Allow: GET, POST, PUT, DELETE, OPTIONS
```

### 7. PATCH 方法测试

#### 7.1 部分更新资源
- **测试内容**：`PATCH /api/data/1`
- **请求体**：
  ```json
  {"value": 789}
  ```
- **Content-Type**：application/json
- **验证点**：
  - PATCH 请求正确构造
  - 只发送需要更新的字段
  - 响应状态码 200

#### 7.2 API 调用
```cpp
std::string body = R"({"value": 789})";
auto result = co_await client.patch("/api/data/1", body, "application/json");
```

#### 7.3 与 PUT 的区别
- **PUT**：完整替换资源
- **PATCH**：部分更新资源

### 8. TRACE 方法测试

#### 8.1 诊断追踪
- **测试内容**：`TRACE /api/data`
- **请求特点**：
  - 无请求体
  - 用于诊断
  - 服务器回显请求
- **验证点**：
  - TRACE 请求发送成功
  - 响应状态码 200
  - 响应体包含请求信息

#### 8.2 API 调用
```cpp
auto result = co_await client.trace("/api/data");
```

### 9. CONNECT 方法测试

#### 9.1 建立隧道
- **测试内容**：`CONNECT example.com:443`
- **请求特点**：
  - 用于 HTTPS 代理
  - URI 为 host:port 格式
- **验证点**：
  - CONNECT 请求发送成功
  - 响应状态码 200（隧道建立）
  - 可以开始传输数据

#### 9.2 API 调用
```cpp
auto result = co_await client.tunnel("example.com:443");
```

## 测试用例列表

| 编号 | HTTP 方法 | URI | Body | 预期状态码 | 结果 |
|------|----------|-----|------|-----------|------|
| 1 | GET | /api/data | 无 | 200 | ✓ |
| 2 | POST | /api/data | JSON | 200 | ✓ |
| 3 | PUT | /api/data/1 | JSON | 200 | ✓ |
| 4 | DELETE | /api/data/1 | 无 | 200/204 | ✓ |
| 5 | HEAD | /api/data | 无 | 200 | ✓ |
| 6 | OPTIONS | /api/data | 无 | 200/204 | ✓ |
| 7 | PATCH | /api/data/1 | JSON | 200 | ✓ |
| 8 | TRACE | /api/data | 无 | 200 | ✓ |
| 9 | CONNECT | example.com:443 | 无 | 200 | ✓ |

## 测试代码位置

- **文件路径**：`/Users/gongzhijie/Desktop/projects/git/galay-http/test/T8-HttpMethods.cc`
- **代码行数**：558 行
- **主要函数**：
  - `testGetMethod()`：GET 测试
  - `testPostMethod()`：POST 测试
  - `testPutMethod()`：PUT 测试
  - `testDeleteMethod()`：DELETE 测试
  - `testHeadMethod()`：HEAD 测试
  - `testOptionsMethod()`：OPTIONS 测试
  - `testPatchMethod()`：PATCH 测试
  - `testTraceMethod()`：TRACE 测试
  - `testConnectMethod()`：CONNECT 测试

## HttpClient API 说明

### 所有方法的 API

```cpp
class HttpClient {
public:
    // GET 请求
    Awaitable<std::expected<std::optional<HttpResponse>, HttpError>>
        get(const std::string& uri);

    // POST 请求
    Awaitable<std::expected<std::optional<HttpResponse>, HttpError>>
        post(const std::string& uri,
             const std::string& body,
             const std::string& content_type = "text/plain");

    // PUT 请求
    Awaitable<std::expected<std::optional<HttpResponse>, HttpError>>
        put(const std::string& uri,
            const std::string& body,
            const std::string& content_type = "text/plain");

    // DELETE 请求
    Awaitable<std::expected<std::optional<HttpResponse>, HttpError>>
        del(const std::string& uri);

    // HEAD 请求
    Awaitable<std::expected<std::optional<HttpResponse>, HttpError>>
        head(const std::string& uri);

    // OPTIONS 请求
    Awaitable<std::expected<std::optional<HttpResponse>, HttpError>>
        options(const std::string& uri);

    // PATCH 请求
    Awaitable<std::expected<std::optional<HttpResponse>, HttpError>>
        patch(const std::string& uri,
              const std::string& body,
              const std::string& content_type = "text/plain");

    // TRACE 请求
    Awaitable<std::expected<std::optional<HttpResponse>, HttpError>>
        trace(const std::string& uri);

    // CONNECT 请求（建立隧道）
    Awaitable<std::expected<std::optional<HttpResponse>, HttpError>>
        tunnel(const std::string& host_port);
};
```

## 运行测试

### 前置条件

**需要测试服务器**：

```bash
# 终端 1：启动服务器
./test/T5-HttpServer
```

### 编译测试

```bash
cd build
cmake ..
make T8-HttpMethods
```

### 运行测试

```bash
# 终端 2：运行测试
./test/T8-HttpMethods
```

### 预期输出

```
========================================
HTTP Methods Test
========================================

Note: This test requires a test server running on 127.0.0.1:8080

=== Test 1: GET Method ===
✓ Connected to server
✓ GET request succeeded
  Status: 200
  Body length: 123 bytes
  Loops: 2

=== Test 2: POST Method ===
✓ Connected to server
✓ POST request succeeded
  Status: 200
  Loops: 2

...

========================================
Summary: All HTTP Methods Tested
========================================

✓ GET    - Retrieve resource
✓ POST   - Create resource
✓ PUT    - Update resource
✓ DELETE - Delete resource
✓ HEAD   - Get resource metadata
✓ OPTIONS - Query supported methods
✓ PATCH  - Partial update
✓ TRACE  - Diagnostic trace
✓ CONNECT - Establish tunnel
========================================
```

## 测试结论

### 功能验证

✅ **GET 方法**：正确获取资源
✅ **POST 方法**：正确创建资源
✅ **PUT 方法**：正确更新资源
✅ **DELETE 方法**：正确删除资源
✅ **HEAD 方法**：正确获取元数据
✅ **OPTIONS 方法**：正确查询方法
✅ **PATCH 方法**：正确部分更新
✅ **TRACE 方法**：正确诊断追踪
✅ **CONNECT 方法**：正确建立隧道

### HTTP 方法使用场景

| 方法 | RESTful API | 常见用途 |
|------|------------|---------|
| GET | 查询 | 获取列表、详情 |
| POST | 创建 | 提交表单、上传文件 |
| PUT | 完整更新 | 替换整个资源 |
| DELETE | 删除 | 删除资源 |
| HEAD | 元数据 | 检查资源是否存在 |
| OPTIONS | 预检 | CORS 预检请求 |
| PATCH | 部分更新 | 只更新部分字段 |
| TRACE | 诊断 | 调试代理链 |
| CONNECT | 隧道 | HTTPS 代理 |

### RESTful API 设计

**标准 RESTful 映射**：
```
GET    /api/users       - 获取用户列表
GET    /api/users/1     - 获取用户详情
POST   /api/users       - 创建用户
PUT    /api/users/1     - 完整更新用户
PATCH  /api/users/1     - 部分更新用户
DELETE /api/users/1     - 删除用户
```

### 幂等性说明

**幂等方法**（多次调用结果相同）：
- GET、HEAD、OPTIONS、TRACE
- PUT、DELETE

**非幂等方法**：
- POST、PATCH、CONNECT

### 安全性说明

**安全方法**（不修改服务器状态）：
- GET、HEAD、OPTIONS、TRACE

**不安全方法**：
- POST、PUT、DELETE、PATCH、CONNECT

### 适用场景

1. **RESTful API 客户端**：完整支持所有 HTTP 方法
2. **Web 爬虫**：GET、HEAD 方法
3. **API 测试工具**：所有方法
4. **代理服务器**：CONNECT 方法

### 注意事项

1. **服务器支持**：并非所有服务器都支持所有方法
2. **CORS**：跨域请求需要 OPTIONS 预检
3. **幂等性**：PUT/DELETE 应该是幂等的
4. **安全性**：GET/HEAD 不应修改服务器状态

---

**测试日期**：2026-01-29
**测试人员**：galay-http 开发团队
**文档版本**：v1.0
