# HTTP Reader/Writer 测试说明

## 测试程序说明

本测试将 HttpReader 和 HttpWriter 的功能拆分为两个独立的程序进行测试：

### 1. test_reader_writer_server
- **功能**: HTTP Echo 服务器
- **监听地址**: 127.0.0.1:9999
- **作用**:
  - 接收客户端的HTTP请求
  - 使用 HttpReader 解析请求
  - 使用 HttpWriter 发送响应
  - 响应内容为请求路径的回显

### 2. test_reader_writer_client
- **功能**: HTTP 测试客户端
- **作用**:
  - 连接到服务器
  - 发送5个不同的HTTP请求
  - 验证响应内容
  - 输出测试结果

## 运行测试

### 步骤1: 启动服务器

在第一个终端窗口中运行：

```bash
cd build
./test/test_reader_writer_server
```

**预期输出**:
```
========================================
HTTP Reader/Writer Test - Server
========================================

Scheduler started
=== HTTP Reader/Writer Test Server ===
Starting server...
Server listening on 127.0.0.1:9999
Waiting for client connections...
Server is ready. Press Ctrl+C to stop.
```

### 步骤2: 运行客户端测试

在第二个终端窗口中运行：

```bash
cd build
./test/test_reader_writer_client
```

**预期输出**:
```
========================================
HTTP Reader/Writer Test - Client
========================================

Make sure the server is running on 127.0.0.1:9999
You can start it with: ./test_reader_writer_server

Scheduler started

=== Test #1: /test ===
Test #1: Connected to server
Test #1: Request sent: 101 bytes
Test #1: Response received: 110 bytes
Test #1: Response content:
HTTP/1.1 200 OK
Content-Type: text/plain
Server: galay-http-test/1.0
Content-Length: 23

Echo: /test
Request #1
Test #1 PASSED

=== Test #2: /api/users?id=123 ===
...

========================================
Test Results:
  Passed: 5
  Failed: 0
  Total:  5
========================================
```

### 步骤3: 查看服务器日志

在服务器终端中，你会看到：

```
Client connected from 127.0.0.1:xxxxx
Request #1 received: 0 /test
Response sent: 110 bytes
Connection closed

Client connected from 127.0.0.1:xxxxx
Request #2 received: 0 /api/users?id=123
Response sent: 135 bytes
Connection closed

...
```

## 测试内容

客户端会发送以下5个测试请求：

1. **简单路径**: `GET /test`
2. **带参数的路径**: `GET /api/users?id=123`
3. **长路径**: `GET /very/long/path/to/resource`
4. **根路径**: `GET /`
5. **特殊字符路径**: `GET /test%20path`

每个测试都会验证：
- ✅ 连接成功
- ✅ 请求发送成功
- ✅ 响应接收成功
- ✅ 响应内容正确（包含 "HTTP/1.1 200 OK" 和 "Echo: [路径]"）

## 手动测试

你也可以使用 curl 手动测试服务器：

```bash
# 测试1
curl http://127.0.0.1:9999/test

# 测试2
curl http://127.0.0.1:9999/api/users?id=123

# 测试3
curl -v http://127.0.0.1:9999/hello
```

## 验证点

### HttpReader 验证
- ✅ 正确解析HTTP请求行（方法、路径、版本）
- ✅ 正确解析HTTP请求头
- ✅ 增量解析功能正常
- ✅ 使用 RingBuffer 进行零拷贝缓冲

### HttpWriter 验证
- ✅ 正确序列化HTTP响应
- ✅ 异步发送功能正常
- ✅ 使用 WritevAwaitable 进行零拷贝发送

## 故障排查

### 问题1: 客户端连接失败
**原因**: 服务器未启动或端口被占用

**解决方案**:
```bash
# 检查端口是否被占用
lsof -i :9999

# 如果被占用，杀死进程或更换端口
```

### 问题2: 测试失败
**原因**: 响应内容不符合预期

**解决方案**:
- 查看服务器日志，确认请求是否正确接收
- 查看客户端输出的响应内容
- 检查 HttpReader 和 HttpWriter 的 DEBUG 日志

### 问题3: 服务器崩溃
**原因**: 可能是解析错误或内存问题

**解决方案**:
- 使用 gdb 调试：`gdb ./test_reader_writer_server`
- 查看 core dump 文件
- 检查日志中的错误信息

## 性能测试

使用 wrk 进行压力测试：

```bash
# 安装 wrk (macOS)
brew install wrk

# 运行压测
wrk -t4 -c100 -d10s http://127.0.0.1:9999/test
```

**预期结果**:
- 无错误
- 高吞吐量
- 低延迟

## 总结

这个测试验证了：
1. ✅ HttpReader 能正确解析各种HTTP请求
2. ✅ HttpWriter 能正确发送HTTP响应
3. ✅ 异步IO功能正常工作
4. ✅ 零拷贝缓冲机制有效
5. ✅ 多连接并发处理正常
