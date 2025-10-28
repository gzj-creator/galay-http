# HTTP/2 测试指南

## 测试文件

- `test_http2.html` - HTTP/2 功能测试页面
- `test_http2_server.cc` - HTTP/2 测试服务器
- `test_hpack.cc` - HPACK 压缩单元测试

## 快速开始

### 1. 编译测试程序

```bash
cd build
cmake ..
make test_http2_server test_hpack
```

### 2. 运行 HPACK 单元测试

```bash
cd build/test
./test_hpack
```

这将测试：
- ✅ 哈夫曼编码/解码
- ✅ HPACK 静态表
- ✅ HPACK 动态表
- ✅ HPACK 编码器
- ✅ HPACK 解码器
- ✅ HTTP/2 HEADERS 帧

### 3. 启动 HTTP/2 服务器

```bash
cd build/test
./test_http2_server
```

服务器将在 `http://localhost:8080` 启动。

### 4. 使用 HTML 测试页面

#### 方法 1: 使用简单的 HTTP 服务器

```bash
# 使用 Python 启动一个简单的 HTTP 服务器
cd test
python3 -m http.server 8000

# 或使用 Node.js
npx http-server -p 8000
```

然后在浏览器中打开: `http://localhost:8000/test_http2.html`

#### 方法 2: 直接在浏览器中打开

```bash
# macOS
open test/test_http2.html

# Linux
xdg-open test/test_http2.html

# Windows
start test/test_http2.html
```

## 测试功能

HTML 测试页面提供以下测试：

### 1. 简单请求测试
- 发送单个 GET 请求
- 测试基本的 HTTP/2 连接

### 2. 并发请求测试
- 同时发送 10 个请求
- 测试 HTTP/2 多路复用功能
- 观察并发性能

### 3. 大头部测试
- 发送包含大量头部字段的请求
- 测试 HPACK 头部压缩
- 观察压缩效果

### 4. 流优先级测试
- 发送不同优先级的请求
- 测试 HTTP/2 流优先级功能

## 使用 curl 测试 HTTP/2

### 测试 h2c (HTTP/2 over cleartext)

```bash
# 发送简单请求
curl --http2-prior-knowledge http://localhost:8080/api/test

# 显示详细信息
curl -v --http2-prior-knowledge http://localhost:8080/api/test

# 发送 POST 请求
curl --http2-prior-knowledge \
  -X POST \
  -H "Content-Type: application/json" \
  -d '{"test": "data"}' \
  http://localhost:8080/api/data

# 发送大头部
curl --http2-prior-knowledge \
  -H "X-Custom-1: value1" \
  -H "X-Custom-2: value2" \
  -H "X-Custom-3: value3" \
  http://localhost:8080/api/headers
```

### 测试 SETTINGS 帧

```bash
# 使用 h2load 进行压力测试（需要安装 nghttp2）
h2load -n 1000 -c 10 http://localhost:8080/

# 参数说明:
# -n: 总请求数
# -c: 并发连接数
```

## 监控工具

### 使用 Wireshark 抓包

1. 启动 Wireshark
2. 选择 `Loopback: lo0` 接口
3. 过滤器: `tcp.port == 8080`
4. 右键数据包 -> Decode As -> HTTP/2
5. 观察 HTTP/2 帧（HEADERS, DATA, SETTINGS, PING 等）

### 使用 Chrome DevTools

1. 打开 Chrome 浏览器
2. 按 F12 打开开发者工具
3. 切换到 "Network" 标签
4. 访问测试页面并执行测试
5. 查看：
   - Protocol: h2 或 http/1.1
   - Timing: 查看请求时间线
   - Headers: 查看请求/响应头

## HTTP/2 特性验证

### 1. 协议检测

```bash
# 使用 nghttp 测试
nghttp -v http://localhost:8080/

# 查看协议版本
curl -I --http2-prior-knowledge http://localhost:8080/
```

### 2. HPACK 压缩验证

观察日志中的头部块大小，相比 HTTP/1.1 应该有明显减少。

### 3. 多路复用验证

同时发送多个请求，观察它们是否在同一个 TCP 连接上传输。

### 4. 流控制验证

发送大量数据，观察 WINDOW_UPDATE 帧的使用。

## 性能测试

### 基准测试

```bash
# 使用 h2load 进行基准测试
h2load -n 10000 -c 100 -m 10 http://localhost:8080/

# 参数说明:
# -n: 总请求数
# -c: 并发连接数
# -m: 每个连接的最大并发流数
```

### 对比 HTTP/1.1 vs HTTP/2

```bash
# HTTP/1.1
curl -w "@curl-format.txt" http://localhost:8080/api/test

# HTTP/2
curl --http2-prior-knowledge -w "@curl-format.txt" http://localhost:8080/api/test
```

curl-format.txt 内容:
```
time_namelookup:  %{time_namelookup}\n
time_connect:     %{time_connect}\n
time_starttransfer: %{time_starttransfer}\n
time_total:       %{time_total}\n
```

## 已知限制

1. **浏览器限制**: 
   - 浏览器原生支持的 HTTP/2 (h2) 需要 HTTPS
   - 测试时使用 h2c (HTTP/2 over cleartext)

2. **CORS**: 
   - 跨域请求需要服务器支持 CORS 头部

3. **服务器推送**: 
   - 当前测试服务器不支持 Server Push

## 故障排查

### 问题: 浏览器显示 "http/1.1"

**解决方案**: 
- 浏览器不支持 h2c，需要使用 HTTPS 或使用 curl 测试
- 使用 `curl --http2-prior-knowledge` 强制使用 HTTP/2

### 问题: 请求失败

**检查**:
1. 服务器是否正在运行
2. 端口 8080 是否被占用
3. 防火墙设置

### 问题: HPACK 解码错误

**检查**:
1. 查看服务器日志中的错误信息
2. 使用 test_hpack 单元测试验证 HPACK 功能

## 参考资源

- [RFC 7540 - HTTP/2](https://tools.ietf.org/html/rfc7540)
- [RFC 7541 - HPACK](https://tools.ietf.org/html/rfc7541)
- [HTTP/2 官网](https://http2.github.io/)
- [nghttp2 文档](https://nghttp2.org/)

