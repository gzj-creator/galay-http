# Example

目录结构（参考 galay-rpc）：

- `common/`：示例公共配置
- `include/`：示例实现（E1~E12）

保留示例类型：

- 各协议 Echo：HTTP / HTTPS / WS / WSS / H2c
- 静态服务器
- Proxy

构建：

```bash
cmake -S . -B build -DBUILD_EXAMPLES=ON
cmake --build build -j
```

SSL 示例需启用：

```bash
cmake -S . -B build_ssl -DBUILD_EXAMPLES=ON -DGALAY_HTTP_ENABLE_SSL=ON
cmake --build build_ssl -j
```
