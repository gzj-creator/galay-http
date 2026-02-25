# Example

目录结构（参考 galay-rpc）：

- `common/`：示例公共配置
- `include/`：示例实现（E1~E12）

保留示例类型：

- 各协议 Echo：HTTP / HTTPS / WS / WSS / H2c
- 静态服务器
- Proxy

`E12-HttpProxy` 支持 mount 集成：可以将某个前缀（默认 `/static`）作为本地静态目录，其余路径转发到 upstream。

构建：

```bash
cmake -S . -B build -DBUILD_EXAMPLES=ON
cmake --build build --parallel
```

SSL 示例需启用：

```bash
cmake -S . -B build_ssl -DBUILD_EXAMPLES=ON -DGALAY_HTTP_ENABLE_SSL=ON
cmake --build build_ssl --parallel
```

运行（Proxy + Mount）：

```bash
# 终端 1：upstream
./build/examples/E1-EchoServer 8080

# 终端 2：proxy，挂载 /static -> ./html（Nginx try_files 风格）
./build/examples/E12-HttpProxy 8081 127.0.0.1 8080 /static ./html nginx

# 走代理转发到 upstream
curl -X POST http://127.0.0.1:8081/echo -d "via proxy"

# 走本地 mount
curl http://127.0.0.1:8081/static/ResumeDownload.html
```
