# HttpRouter 使用文档

## 概述

`HttpRouter` 是 galay-http 框架的高性能路由器，采用 **Drogon 混合策略**实现，支持精确匹配、路径参数和通配符匹配。

## 核心特性

### 1. 混合路由策略
- **精确匹配**：使用 `unordered_map`，O(1) 查找性能
- **模糊匹配**：使用 Trie 树，O(k) 查找性能（k 为路径段数）

### 2. 支持的路由模式

#### 精确路径
```cpp
router.addHandler<HttpMethod::GET>("/api/users", handler);
```

#### 路径参数
```cpp
// 单个参数
router.addHandler<HttpMethod::GET>("/user/:id", handler);

// 多个参数
router.addHandler<HttpMethod::GET>("/user/:userId/posts/:postId", handler);
```

#### 通配符
```cpp
// 单段通配符：匹配一个路径段
router.addHandler<HttpMethod::GET>("/static/*", handler);

// 贪婪通配符：匹配多个路径段
router.addHandler<HttpMethod::GET>("/files/**", handler);
```

### 3. 多 HTTP 方法支持
```cpp
// 为同一路径注册多个方法
router.addHandler<HttpMethod::GET, HttpMethod::POST, HttpMethod::PUT>(
    "/api/resource", handler);
```

## API 参考

### 类型定义

```cpp
// 路由处理器类型
using HttpRouteHandler = std::function<Coroutine(HttpConn&, HttpRequest)>;

// 路由匹配结果
struct RouteMatch {
    HttpRouteHandler* handler;                    // 处理器指针
    std::map<std::string, std::string> params;    // 路径参数
};
```

### 主要方法

#### addHandler
```cpp
template<HttpMethod... Methods>
void addHandler(const std::string& path, HttpRouteHandler handler);
```
添加路由处理器，支持可变参数模板指定多个 HTTP 方法。

**参数：**
- `path`: 路由路径（支持精确路径、参数、通配符）
- `handler`: 处理函数

**示例：**
```cpp
router.addHandler<HttpMethod::GET>("/api/users", handler);
router.addHandler<HttpMethod::GET, HttpMethod::POST>("/api/posts", handler);
```

#### findHandler
```cpp
RouteMatch findHandler(HttpMethod method, const std::string& path);
```
查找匹配的路由处理器。

**参数：**
- `method`: HTTP 方法
- `path`: 请求路径

**返回：**
- `RouteMatch`: 包含处理器指针和提取的路径参数

**示例：**
```cpp
auto match = router.findHandler(HttpMethod::GET, "/user/123");
if (match.handler) {
    std::string userId = match.params["id"];  // "123"
    co_await (*match.handler)(conn, request);
}
```

#### removeHandler
```cpp
bool removeHandler(HttpMethod method, const std::string& path);
```
移除指定的路由。

**返回：**
- `bool`: 是否成功移除

#### clear
```cpp
void clear();
```
清空所有路由。

#### size
```cpp
size_t size() const;
```
获取已注册的路由数量。

## 使用示例

### 基本用法

```cpp
#include "galay-http/kernel/http/HttpRouter.h"

using namespace galay::http;

// 创建路由器
HttpRouter router;

// 定义处理器
Coroutine getUsersHandler(HttpConn& conn, HttpRequest req) {
    auto writer = conn.getWriter();

    HttpResponse response;
    response.setBodyStr(R"({"users": []})");

    co_await writer.sendResponse(response);
    co_return;
}

Coroutine getUserByIdHandler(HttpConn& conn, HttpRequest req) {
    // 路径参数会在 findHandler 返回的 RouteMatch 中
    auto writer = conn.getWriter();

    HttpResponse response;
    response.setBodyStr(R"({"user": {"id": "123"}})");

    co_await writer.sendResponse(response);
    co_return;
}

// 注册路由
router.addHandler<HttpMethod::GET>("/api/users", getUsersHandler);
router.addHandler<HttpMethod::GET>("/user/:id", getUserByIdHandler);
router.addHandler<HttpMethod::POST>("/api/users", createUserHandler);

// 在请求处理中使用
Coroutine handleRequest(HttpConn conn) {
    auto reader = conn.getReader();

    HttpRequest request;
    bool complete = false;
    while (!complete) {
        auto result = co_await reader.getRequest(request);
        if (!result) {
            co_await conn.close();
            co_return;
        }
        complete = result.value();
    }

    // 查找路由
    auto match = router.findHandler(
        request.header().method(),
        request.header().uri()
    );

    if (match.handler) {
        // 调用处理器
        co_await (*match.handler)(conn, std::move(request));
    } else {
        // 404 Not Found
        auto writer = conn.getWriter();
        HttpResponse response;
        response.header().code() = HttpStatusCode::NotFound_404;
        response.setBodyStr("404 Not Found");
        co_await writer.sendResponse(response);
    }

    co_await conn.close();
    co_return;
}
```

### RESTful API 示例

```cpp
HttpRouter router;

// Users API
router.addHandler<HttpMethod::GET>("/api/users", listUsers);
router.addHandler<HttpMethod::POST>("/api/users", createUser);
router.addHandler<HttpMethod::GET>("/api/users/:id", getUser);
router.addHandler<HttpMethod::PUT>("/api/users/:id", updateUser);
router.addHandler<HttpMethod::DELETE>("/api/users/:id", deleteUser);

// Posts API
router.addHandler<HttpMethod::GET>("/api/users/:userId/posts", getUserPosts);
router.addHandler<HttpMethod::POST>("/api/users/:userId/posts", createPost);
router.addHandler<HttpMethod::GET>("/api/posts/:postId", getPost);

// Static files
router.addHandler<HttpMethod::GET>("/static/**", serveStaticFiles);
```

### 路径参数提取

```cpp
Coroutine getUserHandler(HttpConn& conn, HttpRequest req) {
    // 注意：路径参数需要在路由匹配时提取
    // 在实际使用中，需要将 params 传递给处理器
    co_return;
}

// 在请求处理中
auto match = router.findHandler(HttpMethod::GET, "/user/123/posts/456");
if (match.handler) {
    // match.params["userId"] == "123"
    // match.params["postId"] == "456"

    // 可以将参数传递给处理器或存储在 request 中
    co_await (*match.handler)(conn, request);
}
```

## 匹配优先级

路由匹配按以下优先级进行：

1. **精确匹配** (最高优先级)
   - `/api/users` 精确匹配 `/api/users`

2. **参数匹配**
   - `/user/:id` 匹配 `/user/123`

3. **通配符匹配**
   - `/static/*` 匹配 `/static/css`
   - `/files/**` 匹配 `/files/a/b/c`

**示例：**
```cpp
router.addHandler<HttpMethod::GET>("/api/users", exactHandler);     // 优先级 1
router.addHandler<HttpMethod::GET>("/api/:resource", paramHandler); // 优先级 2
router.addHandler<HttpMethod::GET>("/api/*", wildcardHandler);      // 优先级 3

// 请求 /api/users 会匹配 exactHandler
// 请求 /api/posts 会匹配 paramHandler (resource="posts")
// 请求 /api/v1 会匹配 wildcardHandler
```

## 性能特性

### 基准测试结果

| 场景 | 路由数量 | 操作 | 性能 |
|------|---------|------|------|
| 精确匹配 | 1,000 | 添加 | 755K ops/s |
| 精确匹配 | 1,000 | 查找 | 2.94M ops/s (0.34 μs) |
| 参数匹配 | 100 | 查找 | 443K ops/s (2.26 μs) |
| 混合路由 | 1,000 | 查找 | 527K ops/s (1.90 μs) |
| 可扩展性 | 10,000 | 查找 | 3.63M ops/s (0.28 μs) |

### 性能特点

1. **精确匹配极快**：O(1) 时间复杂度，平均 0.34 微秒
2. **参数匹配高效**：使用 Trie 树，O(k) 时间复杂度
3. **可扩展性好**：10,000 条路由仍保持高性能
4. **内存效率高**：估计约 100KB/1000 条路由

### 性能优化建议

1. **优先使用精确匹配**：精确路径性能最佳
2. **避免过多通配符**：通配符匹配需要遍历
3. **合理组织路由**：将常用路由放在前面
4. **批量注册路由**：减少重复操作

## 注意事项

### 1. 路径格式
- 路径必须以 `/` 开头
- 多个连续的 `/` 会被合并
- 尾部 `/` 会被保留（`/api/users` 和 `/api/users/` 是不同的路由）

### 2. 参数命名
- 参数名必须以 `:` 开头
- 参数名只能包含字母、数字和下划线
- 同一路径中参数名不能重复

### 3. 通配符使用
- `*` 只匹配一个路径段
- `**` 匹配剩余所有路径段
- 通配符必须是路径的最后一段

### 4. 线程安全
- `addHandler` 不是线程安全的，应在启动前注册所有路由
- `findHandler` 是线程安全的，可以在多线程环境中使用

### 5. 处理器生命周期
- 处理器函数会被复制存储
- 确保处理器捕获的变量生命周期正确

## 错误处理

```cpp
// 路由未找到
auto match = router.findHandler(method, path);
if (!match.handler) {
    // 返回 404
    HttpResponse response;
    response.header().code() = HttpStatusCode::NotFound_404;
    response.setBodyStr("404 Not Found");
    co_await writer.sendResponse(response);
}

// 方法不允许
auto match = router.findHandler(HttpMethod::POST, "/api/users");
if (!match.handler) {
    // 可能是方法不匹配，返回 405
    HttpResponse response;
    response.header().code() = HttpStatusCode::MethodNotAllowed_405;
    response.setBodyStr("405 Method Not Allowed");
    co_await writer.sendResponse(response);
}
```

## 与 HttpServer 集成

```cpp
#include "galay-http/kernel/http/HttpServer.h"
#include "galay-http/kernel/http/HttpRouter.h"

// 创建全局路由器
HttpRouter g_router;

// 注册路由
void setupRoutes() {
    g_router.addHandler<HttpMethod::GET>("/", indexHandler);
    g_router.addHandler<HttpMethod::GET>("/api/users", getUsersHandler);
    g_router.addHandler<HttpMethod::POST>("/api/users", createUserHandler);
}

// 请求处理器
Coroutine handleRequest(HttpConn conn) {
    auto reader = conn.getReader();
    auto writer = conn.getWriter();

    HttpRequest request;
    bool complete = false;
    while (!complete) {
        auto result = co_await reader.getRequest(request);
        if (!result) {
            co_await conn.close();
            co_return;
        }
        complete = result.value();
    }

    // 使用路由器
    auto match = g_router.findHandler(
        request.header().method(),
        request.header().uri()
    );

    if (match.handler) {
        co_await (*match.handler)(conn, std::move(request));
    } else {
        HttpResponse response;
        response.header().code() = HttpStatusCode::NotFound_404;
        response.setBodyStr("404 Not Found");
        co_await writer.sendResponse(response);
    }

    co_await conn.close();
    co_return;
}

int main() {
    setupRoutes();

    HttpServerConfig config;
    config.host = "127.0.0.1";
    config.port = 8080;

    HttpServer server(config);
    server.start(handleRequest);

    // 服务器运行...

    return 0;
}
```

## 测试

运行单元测试：
```bash
./build/test/test_http_router
```

运行性能测试：
```bash
./build/benchmark/benchmark_http_router
```

## 参考资料

- [Drogon Framework](https://github.com/drogonframework/drogon) - 参考了其路由实现策略
- [Trie 树数据结构](https://en.wikipedia.org/wiki/Trie) - 模糊匹配的核心算法
- galay-kernel Trie 实现：`/usr/local/include/galay-utils/trie/TrieTree.hpp`
