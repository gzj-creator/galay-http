# 静态文件服务功能说明

## 概述

Galay-HTTP 提供了安全、高效的静态文件服务功能，支持自动路径校验和 Content-Type 检测。

## 快速开始

### 基本用法

```cpp
#include "galay/kernel/runtime/Runtime.h"
#include "galay-http/server/HttpServer.h"
#include "galay-http/kernel/HttpRouter.h"

using namespace galay;
using namespace galay::http;

int main() {
    RuntimeBuilder runtimeBuilder;
    auto runtime = runtimeBuilder.build();
    runtime.start();
    
    HttpServerBuilder builder;
    HttpServer server = builder.build();
    server.listen(Host("0.0.0.0", 8080));
    
    HttpRouter router;
    
    // 挂载静态文件目录
    // 注意：mount() 会立即验证路径是否存在，不存在会抛出异常
    try {
        router.mount("/static", "./public");
    } catch (const std::exception& e) {
        std::cerr << "Mount failed: " << e.what() << std::endl;
        return 1;
    }
    
    server.run(runtime, router);
    server.wait();
    server.stop();
    
    return 0;
}
```

### 路径映射规则

`mount()` 方法会自动处理路径：

```cpp
// 以下调用都会被规范化
router.mount("/static", "./public");    // -> "/static/*"
router.mount("/static/", "./public");   // -> "/static/*"  
router.mount("/static/*", "./public");  // -> "/static/*"
```

### 请求示例

```bash
# 服务器配置
router.mount("/static", "./public");

# 请求
GET /static/css/style.css
# -> 读取文件: ./public/css/style.css

GET /static/js/app.js  
# -> 读取文件: ./public/js/app.js

GET /static/images/logo.png
# -> 读取文件: ./public/images/logo.png

GET /static/
# -> 读取文件: ./public/index.html (默认文件)
```

## 安全特性

### 1. 启动时路径验证

`mount()` 函数会在服务器启动时验证路径：

```cpp
// mount() 执行的验证步骤：
1. 检查路径是否存在 -> 不存在抛出异常
2. 检查是否为目录 -> 不是目录抛出异常  
3. 规范化为绝对路径 -> 转换为规范化的绝对路径
4. 存储规范化路径 -> 后续请求直接使用
```

**异常信息：**
- `Mount path does not exist: <path>` - 路径不存在
- `Mount path is not a directory: <path>` - 不是目录

这样可以：
- ✅ 在启动时就发现配置错误
- ✅ 避免每次请求都重复路径规范化
- ✅ 提高运行时性能

### 2. 路径遍历攻击防护

系统会自动阻止恶意路径访问：

```bash
# ❌ 以下请求都会被拒绝并返回 403 Forbidden
GET /static/../../../etc/passwd
GET /static/../../config.json
GET /static/%2e%2e%2f%2e%2e%2fconfig
```

### 3. 运行时安全检查流程

```cpp
// 处理请求时的安全检查：
1. 获取请求的文件相对路径 (从 params["*"])
2. 构建完整文件路径 (base_path / requested_file)
3. 规范化完整路径（解析 .. 等）
4. 验证最终路径是否在 base_path 下
5. 检查是否为普通文件（非目录）
6. 读取并返回文件内容
```

## 支持的文件类型

### 自动 Content-Type 检测

系统根据文件扩展名自动设置正确的 Content-Type：

| 扩展名 | Content-Type |
|--------|--------------|
| `.html`, `.htm` | `text/html` |
| `.css` | `text/css` |
| `.js` | `application/javascript` |
| `.json` | `application/json` |
| `.xml` | `application/xml` |
| `.txt` | `text/plain` |
| `.png` | `image/png` |
| `.jpg`, `.jpeg` | `image/jpeg` |
| `.gif` | `image/gif` |
| `.svg` | `image/svg+xml` |
| `.pdf` | `application/pdf` |
| 其他 | `application/octet-stream` |

## HTTP 响应码

| 场景 | 响应码 | 说明 |
|------|--------|------|
| 文件成功读取 | 200 OK | 返回文件内容 |
| 文件不存在 | 404 Not Found | 文件路径不存在 |
| 路径遍历攻击 | 403 Forbidden | 尝试访问目录外的文件 |
| 尝试访问目录 | 403 Forbidden | 只能访问普通文件 |
| 文件读取错误 | 500 Internal Server Error | 文件系统错误 |

## 高级用法

### 挂载多个目录

```cpp
HttpRouter router;

// 静态资源
router.mount("/static", "./public");

// 用户上传的文件
router.mount("/uploads", "./uploads");

// CDN 资源
router.mount("/cdn", "/var/www/cdn");

// 图片资源
router.mount("/images", "./assets/images");
```

### 通配符参数访问

在自定义处理函数中，可以通过 `params["*"]` 获取通配符匹配的内容：

```cpp
Coroutine<nil> customHandler(HttpRequest& request, HttpConnection& conn, 
                             HttpParams params) {
    // 对于请求 /static/css/style.css
    // params["*"] = "css/style.css"
    std::string filePath = params["*"];
    
    // 自定义处理逻辑...
    
    co_return nil();
}
```

## 目录结构示例

```
project/
├── main.cc               # 主程序
├── public/               # 静态文件目录
│   ├── index.html
│   ├── css/
│   │   └── style.css
│   ├── js/
│   │   └── app.js
│   └── images/
│       └── logo.png
└── uploads/              # 用户上传目录
    └── ...
```

## 性能优化建议

1. **使用绝对路径**：避免重复的路径解析
   ```cpp
   router.mount("/static", "/var/www/public");  // 推荐
   router.mount("/static", "./public");         // 需要相对路径解析
   ```

2. **文件缓存**：对于频繁访问的文件，可以在应用层实现缓存

3. **CDN 集成**：将静态文件部署到 CDN，减轻服务器负担

## 注意事项

1. **路径验证时机**：`mount()` 会立即验证路径，如果路径不存在或不是目录会抛出异常
   ```cpp
   try {
       router.mount("/static", "./public");  // 立即验证
   } catch (const std::runtime_error& e) {
       std::cerr << "Error: " << e.what() << std::endl;
   }
   ```

2. **路径必须是目录**：只能挂载目录，不能挂载单个文件

3. **权限设置**：确保进程有读取目录和文件的权限

4. **符号链接**：系统会解析符号链接，注意潜在的安全问题

5. **大文件处理**：当前实现会将整个文件读入内存，超大文件建议使用其他方案

## 错误处理

```cpp
// 系统会捕获并处理以下异常：
try {
    // 文件操作
} catch (const std::filesystem::filesystem_error& e) {
    // 文件系统错误 -> 404 Not Found
} catch (const std::exception& e) {
    // 其他异常 -> 500 Internal Server Error
}
```

## 完整示例

参见测试文件：`test/test_static_files.cc`

```cpp
#include "galay/kernel/runtime/Runtime.h"
#include "kernel/HttpRouter.h"
#include "server/HttpServer.h"
#include "utils/HttpLogger.h"

using namespace galay;
using namespace galay::http;

int main()
{
    HttpLogger::getInstance()->getLogger()->getSpdlogger()
        ->set_level(spdlog::level::level_enum::debug);
    
    RuntimeBuilder runtimeBuilder;
    auto runtime = runtimeBuilder.build();
    runtime.start();
    
    HttpServerBuilder builder;
    HttpServer server = builder.build();
    server.listen(Host("0.0.0.0", 8080));
    
    HttpRouter router;
    router.mount("/static", "./public");
    router.mount("/assets", "./assets");
    
    server.run(runtime, router);
    server.wait();
    server.stop();
    
    return 0;
}
```

## 测试

创建测试文件：
```bash
mkdir -p public/css public/js public/images
echo "<h1>Hello World</h1>" > public/index.html
echo "body { color: red; }" > public/css/style.css
echo "console.log('test');" > public/js/app.js
```

启动服务器并测试：
```bash
./build/test/test_static_files

# 在另一个终端测试
curl http://localhost:8080/static/
curl http://localhost:8080/static/css/style.css
curl http://localhost:8080/static/js/app.js

# 测试安全性
curl http://localhost:8080/static/../../../etc/passwd  # 应返回 403
```

