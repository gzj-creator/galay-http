# T15-Range 和 ETag 服务器测试

## 测试概述

本文档记录 HTTP Range 请求和 ETag 验证功能的测试服务器。该服务器提供静态文件服务，支持断点续传、条件请求和缓存验证。

## 测试目标

验证 HTTP 协议的高级特性，确保能够正确处理：
- Range 请求（断点续传）
- ETag 生成和验证
- If-None-Match 条件请求（304 Not Modified）
- If-Range 条件范围请求
- Content-Range 响应头
- Accept-Ranges 支持声明
- CORS 跨域支持

## 测试场景

### 1. Range 请求测试

#### 1.1 单范围请求
- **测试内容**：请求文件的部分内容
- **请求示例**：
  ```http
  GET /files/test_large.bin HTTP/1.1
  Range: bytes=0-1023
  ```
- **验证点**：
  - 返回 206 Partial Content
  - Content-Range 头正确
  - 返回指定范围的数据

#### 1.2 多范围请求
- **测试内容**：请求文件的多个不连续范围
- **请求示例**：
  ```http
  GET /files/test_large.bin HTTP/1.1
  Range: bytes=0-1023,2048-3071
  ```
- **验证点**：
  - 返回 206 Partial Content
  - 使用 multipart/byteranges 格式
  - 每个范围都正确返回

#### 1.3 无效范围请求
- **测试内容**：请求超出文件大小的范围
- **请求示例**：
  ```http
  GET /files/test_small.bin HTTP/1.1
  Range: bytes=9999999-
  ```
- **验证点**：
  - 返回 416 Range Not Satisfiable
  - Content-Range: bytes */文件大小

### 2. ETag 验证测试

#### 2.1 ETag 生成
- **测试内容**：为文件生成唯一的 ETag
- **生成方式**：基于文件路径、大小、修改时间的哈希
- **验证点**：
  - 每个文件有唯一的 ETag
  - 文件未修改时 ETag 不变
  - 文件修改后 ETag 改变

#### 2.2 If-None-Match（缓存验证）
- **测试内容**：客户端发送缓存的 ETag
- **请求示例**：
  ```http
  GET /files/test_small.bin HTTP/1.1
  If-None-Match: "abc123def456"
  ```
- **验证点**：
  - ETag 匹配 → 返回 304 Not Modified
  - ETag 不匹配 → 返回 200 OK 和完整文件

#### 2.3 If-Match（条件更新）
- **测试内容**：确保文件未被修改才执行操作
- **请求示例**：
  ```http
  PUT /files/test.txt HTTP/1.1
  If-Match: "abc123def456"
  ```
- **验证点**：
  - ETag 匹配 → 执行操作
  - ETag 不匹配 → 返回 412 Precondition Failed

### 3. If-Range 条件范围请求测试

#### 3.1 ETag 匹配
- **测试内容**：If-Range 与 Range 结合使用
- **请求示例**：
  ```http
  GET /files/test_large.bin HTTP/1.1
  Range: bytes=1024-2047
  If-Range: "abc123def456"
  ```
- **验证点**：
  - ETag 匹配 → 返回 206 和指定范围
  - ETag 不匹配 → 返回 200 和完整文件

### 4. CORS 支持测试

#### 4.1 预检请求（OPTIONS）
- **测试内容**：处理 CORS 预检请求
- **请求示例**：
  ```http
  OPTIONS /files/test.bin HTTP/1.1
  Origin: http://localhost:3000
  Access-Control-Request-Method: GET
  ```
- **验证点**：
  - 返回 204 No Content
  - 包含 CORS 响应头
  - Access-Control-Allow-Origin: *

#### 4.2 实际请求
- **测试内容**：跨域 GET 请求
- **验证点**：
  - 响应包含 CORS 头
  - Access-Control-Expose-Headers 正确

### 5. 断点续传场景测试

#### 5.1 下载中断后恢复
- **测试流程**：
  1. 开始下载大文件
  2. 下载到 50% 时中断
  3. 使用 Range 请求继续下载
- **验证点**：
  - 第二次请求从中断点继续
  - 最终文件完整

#### 5.2 多线程下载
- **测试流程**：
  1. 将文件分成多个范围
  2. 并发请求各个范围
  3. 合并所有范围
- **验证点**：
  - 所有范围请求成功
  - 合并后文件完整

### 6. 缓存策略测试

#### 6.1 首次访问
- **测试内容**：第一次请求文件
- **验证点**：
  - 返回 200 OK
  - 包含 ETag 头
  - 包含 Accept-Ranges 头

#### 6.2 缓存命中
- **测试内容**：使用 If-None-Match 请求
- **验证点**：
  - 返回 304 Not Modified
  - 无 body 内容
  - 节省带宽

#### 6.3 缓存失效
- **测试内容**：文件修改后请求
- **验证点**：
  - ETag 不匹配
  - 返回 200 OK 和新内容

## 测试用例列表

| 编号 | 测试用例 | 类型 | 预期结果 |
|------|---------|------|---------|
| 1 | 单范围请求 | Range | ✓ 206 Partial Content |
| 2 | 多范围请求 | Range | ✓ 206 Multipart |
| 3 | 无效范围 | Range | ✓ 416 Range Not Satisfiable |
| 4 | ETag 生成 | ETag | ✓ 唯一标识 |
| 5 | If-None-Match 匹配 | ETag | ✓ 304 Not Modified |
| 6 | If-None-Match 不匹配 | ETag | ✓ 200 OK |
| 7 | If-Range 匹配 | Conditional | ✓ 206 范围响应 |
| 8 | If-Range 不匹配 | Conditional | ✓ 200 完整响应 |
| 9 | CORS 预检 | CORS | ✓ 204 No Content |
| 10 | CORS 实际请求 | CORS | ✓ CORS 头正确 |
| 11 | 断点续传 | Resume | ✓ 续传成功 |
| 12 | 多线程下载 | Concurrent | ✓ 并发成功 |
| 13 | 缓存命中 | Cache | ✓ 304 响应 |
| 14 | 缓存失效 | Cache | ✓ 200 响应 |

## 测试代码位置

- **文件路径**：`/Users/gongzhijie/Desktop/projects/git/galay-http/test/T15-RangeEtagServer.cc`
- **测试函数数量**：2 个（corsHandler, fileHandlerWithCORS）
- **代码行数**：348 行

## 运行测试

### 编译测试

```bash
cd build
cmake ..
make T15-RangeEtagServer
```

### 启动服务器

```bash
./test/T15-RangeEtagServer [port]
# 默认端口: 8080
```

### 预期输出

```
========================================
Range & ETag Test Server
========================================
Creating test file: ./files/test_small.bin (1 MB)
Creating test file: ./files/test_medium.bin (5 MB)
Creating test file: ./files/test_large.bin (10 MB)
========================================
Server is running on http://0.0.0.0:8080
========================================
Test Files (API Endpoints):
  - http://localhost:8080/files/test_small.bin  (1 MB)
  - http://localhost:8080/files/test_medium.bin (5 MB)
  - http://localhost:8080/files/test_large.bin  (10 MB)
========================================
How to Test:
  1. Open html/ResumeDownload.html in browser
  2. Open html/EtagCache.html in browser
  3. Click buttons to test Range and ETag features
========================================
Features:
  ✓ Range requests support
  ✓ ETag validation (If-None-Match, If-Range)
  ✓ 304 Not Modified responses
  ✓ Auto transfer mode (MEMORY/CHUNK/SENDFILE)
========================================
Press Ctrl+C to stop the server
========================================
```

### 使用 curl 测试

#### 测试 Range 请求
```bash
# 请求前 1024 字节
curl -H "Range: bytes=0-1023" http://localhost:8080/files/test_small.bin -v

# 请求从 1024 字节到末尾
curl -H "Range: bytes=1024-" http://localhost:8080/files/test_small.bin -v
```

#### 测试 ETag 缓存
```bash
# 第一次请求，获取 ETag
curl -I http://localhost:8080/files/test_small.bin

# 使用 ETag 请求（应返回 304）
curl -H "If-None-Match: \"etag_value\"" http://localhost:8080/files/test_small.bin -v
```

#### 测试 If-Range
```bash
# 条件范围请求
curl -H "Range: bytes=1024-2047" \
     -H "If-Range: \"etag_value\"" \
     http://localhost:8080/files/test_small.bin -v
```

### 使用浏览器测试

1. 启动服务器
2. 打开 `html/ResumeDownload.html`
3. 点击"开始下载"按钮
4. 点击"暂停"后再点击"继续"测试断点续传
5. 打开 `html/EtagCache.html` 测试缓存

## 测试结论

### 功能验证

✅ **Range 请求支持**：完整支持 HTTP Range 规范
✅ **ETag 生成**：基于文件属性生成唯一标识
✅ **条件请求**：支持 If-None-Match、If-Match、If-Range
✅ **304 响应**：正确返回 Not Modified
✅ **CORS 支持**：完整的跨域资源共享支持
✅ **错误处理**：正确处理无效范围和条件

### HTTP 头部说明

#### 请求头
- **Range**: `bytes=start-end` - 请求指定范围
- **If-None-Match**: `"etag"` - 缓存验证
- **If-Match**: `"etag"` - 条件更新
- **If-Range**: `"etag"` - 条件范围请求

#### 响应头
- **Accept-Ranges**: `bytes` - 声明支持范围请求
- **Content-Range**: `bytes start-end/total` - 范围信息
- **ETag**: `"unique_id"` - 资源标识
- **Access-Control-Allow-Origin**: `*` - CORS 支持

### 应用场景

1. **视频流媒体**：支持拖动进度条
2. **大文件下载**：支持断点续传
3. **移动应用**：节省流量，支持弱网环境
4. **CDN 缓存**：ETag 验证减少回源
5. **多线程下载器**：并发下载多个范围

### 性能优化

- **零拷贝**：使用 sendfile 传输大文件
- **缓存友好**：ETag 支持浏览器缓存
- **带宽节省**：304 响应无 body
- **并发支持**：多个范围请求可并发处理

### 安全考虑

- **范围验证**：防止超出文件大小的请求
- **ETag 验证**：防止并发修改冲突
- **路径安全**：防止路径遍历攻击
- **CORS 配置**：可配置允许的源

### 标准兼容性

- **RFC 7233**：HTTP/1.1 Range Requests
- **RFC 7232**：HTTP/1.1 Conditional Requests
- **RFC 6454**：The Web Origin Concept (CORS)

---

**测试日期**：2026-01-29
**测试人员**：galay-http 开发团队
**文档版本**：v1.0
