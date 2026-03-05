# HTTP Header 解析优化设计文档

**日期**: 2026-03-05
**作者**: Galay HTTP Team
**状态**: 已批准

## 背景

基于性能基准测试（`benchmark/results/20260305-093125-galay-go-rust-http-proto-compare`），发现 Galay HTTP/1.1 性能落后竞品：

- **Rust**: 150,059 RPS
- **Go**: 128,225 RPS
- **Galay**: 116,136 RPS（落后 22.6%）

火焰图分析显示主要瓶颈：
1. `HttpRequestHeader::parseChar` 热点（48 samples）
2. `__tolower` 频繁调用（逐字符大小写转换）
3. `map::insert_or_assign` 开销（每个 header 都查找/插入）

## 优化目标

- **性能提升**: HTTP/1.1 parsing 提升 15-20%
- **减少开销**: 减少 `toLower` 调用 80%+
- **查询优化**: 常见 header 查询从 O(log n) 降到 O(1)
- **兼容性**: 保持现有 API 不变，Client 端兼容老旧服务器

## 设计方案

### 1. 整体架构

**双路径架构**：
- **Fast Path**: 15 个常见 header → 直接字节比较 + 数组存储
- **Slow Path**: 罕见 header → 规范化 + map 存储

**常见 Header 列表**（按频率排序）：

```cpp
enum class CommonHeaderIndex : uint8_t {
    Host = 0,           // 必有
    ContentLength,      // POST/PUT 必有
    ContentType,        // POST/PUT 常见
    UserAgent,          // 客户端必有
    Accept,             // 客户端常见
    AcceptEncoding,     // 客户端常见
    Connection,         // HTTP/1.1 常见
    CacheControl,       // 缓存相关
    Cookie,             // 会话相关
    Authorization,      // 认证相关
    IfModifiedSince,    // 条件请求
    IfNoneMatch,        // 条件请求
    Referer,            // 来源追踪
    AcceptLanguage,     // 国际化
    Range,              // 断点续传
    NotCommon = 255     // 标记非常见
};
```

**存储结构**：

```cpp
class HeaderPair {
public:
    enum class Mode {
        ServerSide,   // 服务端：统一小写
        ClientSide    // 客户端：保留原始
    };

private:
    Mode m_mode;

    // Fast path（仅 ServerSide 使用）
    std::array<std::string, 15> m_commonHeaders;
    std::bitset<15> m_commonHeaderPresent;

    // Slow path / ClientSide 全用这个
    std::map<std::string, std::string> m_headers;
};
```

### 2. 大小写规范

**Server 端**（接收请求/响应）：
- 统一转小写存储和使用
- 符合 HTTP/2 规范（强制小写）
- 简化匹配逻辑（直接 `==` 比较）

**Client 端**（发送请求）：
- 保留用户设置的原始大小写
- 兼容不支持小写的老旧服务器
- 不使用 fast path 优化

**常见 Header 名称映射**（全小写）：

```cpp
static constexpr std::array<std::string_view, 15> names = {
    "host",
    "content-length",
    "content-type",
    "user-agent",
    "accept",
    "accept-encoding",
    "connection",
    "cache-control",
    "cookie",
    "authorization",
    "if-modified-since",
    "if-none-match",
    "referer",
    "accept-language",
    "range"
};
```

### 3. Fast Path 检测逻辑

**快速匹配算法**：

```cpp
CommonHeaderIndex matchCommonHeader(const std::string& key) {
    const size_t len = key.size();
    if (len < 4 || len > 19) return CommonHeaderIndex::NotCommon;

    // 首字符快速分发（假设 key 已是小写）
    switch (key[0]) {
    case 'h':
        if (len == 4 && key == "host")
            return CommonHeaderIndex::Host;
        break;

    case 'c':
        if (len == 14 && key == "content-length")
            return CommonHeaderIndex::ContentLength;
        if (len == 12 && key == "content-type")
            return CommonHeaderIndex::ContentType;
        if (len == 10 && key == "connection")
            return CommonHeaderIndex::Connection;
        if (len == 13 && key == "cache-control")
            return CommonHeaderIndex::CacheControl;
        if (len == 6 && key == "cookie")
            return CommonHeaderIndex::Cookie;
        break;

    case 'u':
        if (len == 10 && key == "user-agent")
            return CommonHeaderIndex::UserAgent;
        break;

    case 'a':
        if (len == 6 && key == "accept")
            return CommonHeaderIndex::Accept;
        if (len == 15) {
            if (key == "accept-encoding")
                return CommonHeaderIndex::AcceptEncoding;
            if (key == "accept-language")
                return CommonHeaderIndex::AcceptLanguage;
        }
        if (len == 13 && key == "authorization")
            return CommonHeaderIndex::Authorization;
        break;

    case 'i':
        if (len == 17 && key == "if-modified-since")
            return CommonHeaderIndex::IfModifiedSince;
        if (len == 13 && key == "if-none-match")
            return CommonHeaderIndex::IfNoneMatch;
        break;

    case 'r':
        if (len == 7 && key == "referer")
            return CommonHeaderIndex::Referer;
        if (len == 5 && key == "range")
            return CommonHeaderIndex::Range;
        break;
    }

    return CommonHeaderIndex::NotCommon;
}
```

**优化点**：
1. 首字符 + 长度过滤（O(1)）
2. 直接 `==` 比较（key 已是小写）
3. 无字符串拷贝和规范化

**Parsing 集成**：

```cpp
// 在 HttpRequestHeader::parseChar() 中
case RequestParseState::HeaderKey:
    if (c == ':') {
        if (m_parseHeaderKey.size() > 256) {
            return kBadRequest;  // 边界保护
        }
        // Server 端：key 已在累积时转成小写
        m_currentCommonHeaderIdx = matchCommonHeader(m_parseHeaderKey);
        m_parseState = RequestParseState::HeaderColon;
    } else if (!isValidHeaderKeyChar(c)) {
        return kBadRequest;  // 非法字符
    } else {
        if (m_parseHeaderKey.size() >= 256) {
            return kBadRequest;
        }
        // Server 端：边解析边转小写
        m_parseHeaderKey += toLowerAsciiChar(c);
    }
    break;
```

### 4. 存储和访问接口

**提交 Header**：

```cpp
void HttpRequestHeader::commitParsedHeaderPair() {
    if (m_parseHeaderKey.empty()) return;

    if (m_headerPairs.mode() == HeaderPair::Mode::ServerSide) {
        // Server 端：使用 fast path
        if (m_currentCommonHeaderIdx != CommonHeaderIndex::NotCommon) {
            m_headerPairs.setCommonHeader(
                m_currentCommonHeaderIdx,
                std::move(m_parseHeaderValue)
            );
        } else {
            // 罕见 header，key 已经是小写
            m_headerPairs.addHeaderPair(
                std::move(m_parseHeaderKey),
                std::move(m_parseHeaderValue)
            );
        }
    } else {
        // Client 端：直接存 map，不转小写
        m_headerPairs.addHeaderPair(
            std::move(m_parseHeaderKey),
            std::move(m_parseHeaderValue)
        );
    }

    m_parseHeaderKey.clear();
    m_parseHeaderValue.clear();
    m_currentCommonHeaderIdx = CommonHeaderIndex::NotCommon;
}
```

**查询接口**：

```cpp
class HeaderPair {
public:
    // 统一查询接口（兼容现有代码）
    std::string getHeaderPair(const std::string& key) const {
        if (m_mode == Mode::ServerSide) {
            // 先尝试 fast path
            CommonHeaderIndex idx = matchCommonHeader(key);
            if (idx != CommonHeaderIndex::NotCommon) {
                auto view = getCommonHeader(idx);
                return std::string(view);
            }
        }

        // Slow path: 查 map
        return getHeaderPairFromMap(key);
    }

    // 遍历所有 headers（用于序列化）
    void forEachHeader(std::function<void(std::string_view, std::string_view)> callback) const {
        if (m_mode == Mode::ServerSide) {
            // 先遍历常见 headers
            for (size_t i = 0; i < 15; ++i) {
                if (m_commonHeaderPresent.test(i)) {
                    callback(getCommonHeaderName(static_cast<CommonHeaderIndex>(i)),
                            m_commonHeaders[i]);
                }
            }
        }
        // 再遍历 map
        for (const auto& [k, v] : m_headers) {
            callback(k, v);
        }
    }

private:
    std::string_view getCommonHeader(CommonHeaderIndex idx) const {
        size_t i = static_cast<size_t>(idx);
        if (m_commonHeaderPresent.test(i)) {
            return m_commonHeaders[i];
        }
        return {};
    }
};
```

### 5. 错误处理和边界情况

**重复 Header 处理**：

```cpp
void HeaderPair::setCommonHeader(CommonHeaderIndex idx, std::string value) {
    size_t i = static_cast<size_t>(idx);

    if (m_commonHeaderPresent.test(i)) {
        // 已存在：追加（用逗号分隔，符合 RFC 7230）
        m_commonHeaders[i] += ", ";
        m_commonHeaders[i] += value;
    } else {
        // 首次设置
        m_commonHeaders[i] = std::move(value);
        m_commonHeaderPresent.set(i);
    }
}
```

**边界保护**：

```cpp
// Header key 长度限制
static constexpr size_t kMaxHeaderKeyLength = 256;

// Header value 长度限制
static constexpr size_t kMaxHeaderValueLength = 8192;

// Header 总数限制
static constexpr size_t kMaxHeaderCount = 100;

// 字符合法性检查
inline bool isValidHeaderKeyChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}
```

## 实现计划

### 需要修改的文件

1. **galay-http/protoc/http/HttpHeader.h**
   - 添加 `CommonHeaderIndex` 枚举
   - 添加 `HeaderPair::Mode` 枚举
   - 修改 `HeaderPair` 类结构

2. **galay-http/protoc/http/HttpHeader.cc**
   - 实现 `matchCommonHeader()`
   - 修改 `HeaderPair` 方法实现

3. **galay-http/protoc/http/HttpRequest.cc**
   - 修改 `HttpRequestHeader::parseChar()`
   - 修改 `commitParsedHeaderPair()`
   - 添加边界检查

4. **galay-http/protoc/http/HttpResponse.cc**
   - 同样修改 Response 解析逻辑

5. **galay-http/kernel/http/HttpClient.h**
   - 确保 Client 使用 `Mode::ClientSide`

6. **test/T-HeaderFastPath.cc**（新建）
   - 单元测试

7. **benchmark/B-HeaderParsing.cc**（新建）
   - 性能基准测试

### 实现步骤

**Phase 1: 基础设施（1-2 天）**
1. 定义 `CommonHeaderIndex` 枚举和名称映射表
2. 修改 `HeaderPair` 添加 array 存储和 Mode
3. 实现 `matchCommonHeader()` 快速匹配函数
4. 编写单元测试验证匹配逻辑

**Phase 2: Server 端集成（2-3 天）**
5. 修改 `HttpRequestHeader::parseChar()` 添加 fast path
6. 修改 `commitParsedHeaderPair()` 分路径存储
7. 修改 `HttpResponseHeader` 同样逻辑
8. 添加边界检查和错误处理
9. 运行现有测试确保兼容性

**Phase 3: Client 端适配（1 天）**
10. 确保 Client 端使用 `Mode::ClientSide`
11. 验证原始大小写保留
12. 测试与不同服务器的兼容性

**Phase 4: 性能验证（1-2 天）**
13. 编写 benchmark 对比优化前后
14. 运行完整的 HTTP server benchmark
15. 分析火焰图确认优化生效
16. 调优热点（如有必要）

**Phase 5: 文档和提交（1 天）**
17. 更新相关文档说明小写规范
18. 编写 commit message
19. 提交代码

**总计**: 6-9 天

## 验收标准

### 功能正确性
- ✅ 所有单元测试通过
- ✅ Server 端统一小写存储和查询
- ✅ Client 端保留原始大小写
- ✅ 重复 header 正确合并
- ✅ 边界保护生效

### 性能提升
- 🎯 HTTP/1.1 parsing 提升 **15-20%**
- 🎯 常见 header 查询从 O(log n) 降到 O(1)
- 🎯 减少 `toLower` 调用次数 **80%+**

### 兼容性
- ✅ 现有 API 不变（`getHeader()` 等）
- ✅ Client 端兼容老旧服务器
- ✅ 与 HTTP/2 实现统一（都用小写）

## 风险和缓解

**风险 1**: 现有代码依赖 Canonical 模式（首字母大写）
- **缓解**: 先搜索所有使用 `NormalizeMode::Canonical` 的地方，评估影响

**风险 2**: HTTP/2 代码可能有不同的 header 处理
- **缓解**: 确保 H2 也使用相同的 `HeaderPair` 类，统一行为

**风险 3**: 性能提升不达预期
- **缓解**: 先用 benchmark 验证，必要时添加 SIMD 优化

## 未来优化方向

如果本次优化后仍需进一步提升，可考虑：

1. **SIMD 加速**：使用 SSE4.2/AVX2 批量处理字符转换
2. **Perfect Hashing**：为常见 header 生成完美哈希函数
3. **零拷贝解析**：直接在接收缓冲区上操作，避免字符串拷贝

## 参考资料

- RFC 7230: HTTP/1.1 Message Syntax and Routing
- RFC 7540: HTTP/2 (Section 8.1.2: HTTP Header Fields)
- Benchmark 结果: `benchmark/results/20260305-093125-galay-go-rust-http-proto-compare`
