# HTTP Header 解析优化实现计划

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 优化 HTTP header 解析性能，通过 fast-path 将常见 header 查询从 O(log n) 降到 O(1)，减少 toLower 调用 80%+，提升 HTTP/1.1 parsing 性能 15-20%。

**Architecture:** 双路径架构 - 15 个常见 header 使用数组存储 + 直接索引，罕见 header 使用现有 map。Server 端统一小写，Client 端保留原始大小写（兼容老旧服务器）。

**Tech Stack:** C++17, STL (array, bitset, map), TDD

---

## Phase 1: 基础设施和数据结构

### Task 1: 添加 CommonHeaderIndex 枚举

**Files:**
- Modify: `galay-http/protoc/http/HttpHeader.h:13-52`

**Step 1: 在 HttpHeader.h 中添加枚举定义**

在 `namespace galay::http {` 后面，`RequestParseState` 之前添加：

```cpp
// 常见 HTTP Header 索引（用于 fast-path 优化）
enum class CommonHeaderIndex : uint8_t {
    Host = 0,
    ContentLength,
    ContentType,
    UserAgent,
    Accept,
    AcceptEncoding,
    Connection,
    CacheControl,
    Cookie,
    Authorization,
    IfModifiedSince,
    IfNoneMatch,
    Referer,
    AcceptLanguage,
    Range,
    NotCommon = 255
};
```

**Step 2: 编译验证**

Run: `cd build && make -j8`
Expected: 编译成功，无错误

**Step 3: Commit**

```bash
git add galay-http/protoc/http/HttpHeader.h
git commit -m "feat(http): 添加 CommonHeaderIndex 枚举用于 fast-path"
```

---

### Task 2: 修改 HeaderPair 添加 Mode 和 fast-path 存储

**Files:**
- Modify: `galay-http/protoc/http/HttpHeader.h:53-83`

**Step 1: 修改 HeaderPair 类定义**

将现有的 `NormalizeMode` 改名为 `Mode`，并修改枚举值：

```cpp
class HeaderPair
{
public:
    enum class Mode {
        ServerSide,   // 服务端：统一小写，使用 fast-path
        ClientSide    // 客户端：保留原始大小写，不使用 fast-path
    };

    explicit HeaderPair(Mode mode = Mode::ServerSide);
    HeaderPair(const HeaderPair& other);
    HeaderPair(HeaderPair&& other);

    // ... 现有方法保持不变 ...

    Mode mode() const { return m_mode; }

    // 新增 fast-path 方法
    void setCommonHeader(CommonHeaderIndex idx, std::string value);
    std::string_view getCommonHeader(CommonHeaderIndex idx) const;
    bool hasCommonHeader(CommonHeaderIndex idx) const;

    // 新增遍历方法
    void forEachHeader(std::function<void(std::string_view, std::string_view)> callback) const;

private:
    Mode m_mode;

    // Fast-path 存储（仅 ServerSide 使用）
    std::array<std::string, 15> m_commonHeaders;
    std::bitset<15> m_commonHeaderPresent;

    // Slow-path 存储
    std::map<std::string, std::string> m_headerPairs;
};
```

**Step 2: 添加必要的头文件**

在文件顶部添加：

```cpp
#include <array>
#include <bitset>
#include <functional>
```

**Step 3: 编译验证（预期失败）**

Run: `cd build && make -j8 2>&1 | head -20`
Expected: 编译失败，提示缺少方法实现

**Step 4: Commit**

```bash
git add galay-http/protoc/http/HttpHeader.h
git commit -m "feat(http): HeaderPair 添加 Mode 和 fast-path 存储结构"
```

---

### Task 3: 实现 matchCommonHeader 快速匹配函数

**Files:**
- Modify: `galay-http/protoc/http/HttpHeader.cc:10-80`

**Step 1: 在匿名命名空间中添加 matchCommonHeader 函数**

在 `namespace galay::http { namespace {` 区域，`toLowerAsciiChar` 函数后面添加：

```cpp
// 快速匹配常见 header（假设 key 已是小写）
CommonHeaderIndex matchCommonHeader(const std::string& key) {
    const size_t len = key.size();
    if (len < 4 || len > 19) return CommonHeaderIndex::NotCommon;

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

// 获取常见 header 的标准名称（小写）
std::string_view getCommonHeaderName(CommonHeaderIndex idx) {
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
    return names[static_cast<size_t>(idx)];
}
```

**Step 2: 编译验证**

Run: `cd build && make -j8`
Expected: 编译成功

**Step 3: Commit**

```bash
git add galay-http/protoc/http/HttpHeader.cc
git commit -m "feat(http): 实现 matchCommonHeader 快速匹配函数"
```

---

### Task 4: 实现 HeaderPair 的 fast-path 方法

**Files:**
- Modify: `galay-http/protoc/http/HttpHeader.cc:200-250`

**Step 1: 实现 setCommonHeader 方法**

在 `HeaderPair` 类的实现区域添加：

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

std::string_view HeaderPair::getCommonHeader(CommonHeaderIndex idx) const {
    size_t i = static_cast<size_t>(idx);
    if (m_commonHeaderPresent.test(i)) {
        return m_commonHeaders[i];
    }
    return {};
}

bool HeaderPair::hasCommonHeader(CommonHeaderIndex idx) const {
    return m_commonHeaderPresent.test(static_cast<size_t>(idx));
}

void HeaderPair::forEachHeader(std::function<void(std::string_view, std::string_view)> callback) const {
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
    for (const auto& [k, v] : m_headerPairs) {
        callback(k, v);
    }
}
```

**Step 2: 修改构造函数初始化 bitset**

找到 `HeaderPair::HeaderPair(Mode mode)` 构造函数，确保初始化：

```cpp
HeaderPair::HeaderPair(Mode mode)
    : m_mode(mode)
    , m_commonHeaderPresent(0)  // 初始化为全 0
{
}
```

**Step 3: 修改拷贝构造和赋值运算符**

确保拷贝 `m_commonHeaders` 和 `m_commonHeaderPresent`。

**Step 4: 编译验证**

Run: `cd build && make -j8`
Expected: 编译成功

**Step 5: Commit**

```bash
git add galay-http/protoc/http/HttpHeader.cc
git commit -m "feat(http): 实现 HeaderPair fast-path 存储方法"
```

---

### Task 5: 编写 matchCommonHeader 单元测试

**Files:**
- Create: `test/T39-HeaderFastPath.cc`

**Step 1: 创建测试文件**

```cpp
#include "../galay-http/protoc/http/HttpHeader.h"
#include <gtest/gtest.h>

using namespace galay::http;

// 需要访问内部函数，暂时用 friend 或者测试辅助函数
// 这里先测试公开接口

TEST(HeaderFastPath, SetAndGetCommonHeader) {
    HeaderPair headers(HeaderPair::Mode::ServerSide);

    // 测试设置常见 header
    headers.setCommonHeader(CommonHeaderIndex::Host, "example.com");
    headers.setCommonHeader(CommonHeaderIndex::ContentLength, "123");

    // 测试获取
    EXPECT_EQ(headers.getCommonHeader(CommonHeaderIndex::Host), "example.com");
    EXPECT_EQ(headers.getCommonHeader(CommonHeaderIndex::ContentLength), "123");

    // 测试不存在的
    EXPECT_EQ(headers.getCommonHeader(CommonHeaderIndex::Cookie), "");
}

TEST(HeaderFastPath, DuplicateHeaderMerge) {
    HeaderPair headers(HeaderPair::Mode::ServerSide);

    headers.setCommonHeader(CommonHeaderIndex::Accept, "text/html");
    headers.setCommonHeader(CommonHeaderIndex::Accept, "application/json");

    EXPECT_EQ(headers.getCommonHeader(CommonHeaderIndex::Accept), "text/html, application/json");
}

TEST(HeaderFastPath, HasCommonHeader) {
    HeaderPair headers(HeaderPair::Mode::ServerSide);

    EXPECT_FALSE(headers.hasCommonHeader(CommonHeaderIndex::Host));

    headers.setCommonHeader(CommonHeaderIndex::Host, "example.com");

    EXPECT_TRUE(headers.hasCommonHeader(CommonHeaderIndex::Host));
    EXPECT_FALSE(headers.hasCommonHeader(CommonHeaderIndex::Cookie));
}

TEST(HeaderFastPath, ForEachHeader) {
    HeaderPair headers(HeaderPair::Mode::ServerSide);

    headers.setCommonHeader(CommonHeaderIndex::Host, "example.com");
    headers.setCommonHeader(CommonHeaderIndex::ContentLength, "100");
    headers.addHeaderPair("x-custom", "value");

    std::vector<std::pair<std::string, std::string>> result;
    headers.forEachHeader([&](std::string_view k, std::string_view v) {
        result.emplace_back(k, v);
    });

    EXPECT_EQ(result.size(), 3);
    // 验证包含所有 header
    bool has_host = false, has_length = false, has_custom = false;
    for (const auto& [k, v] : result) {
        if (k == "host" && v == "example.com") has_host = true;
        if (k == "content-length" && v == "100") has_length = true;
        if (k == "x-custom" && v == "value") has_custom = true;
    }
    EXPECT_TRUE(has_host);
    EXPECT_TRUE(has_length);
    EXPECT_TRUE(has_custom);
}
```

**Step 2: 添加到 CMakeLists.txt**

在 `test/CMakeLists.txt` 中添加：

```cmake
add_executable(T39-HeaderFastPath T39-HeaderFastPath.cc)
target_link_libraries(T39-HeaderFastPath galay-http GTest::gtest_main)
add_test(NAME T39-HeaderFastPath COMMAND T39-HeaderFastPath)
```

**Step 3: 编译并运行测试**

Run: `cd build && make T39-HeaderFastPath && ./test/T39-HeaderFastPath`
Expected: 所有测试通过

**Step 4: Commit**

```bash
git add test/T39-HeaderFastPath.cc test/CMakeLists.txt
git commit -m "test(http): 添加 HeaderPair fast-path 单元测试"
```

---

## Phase 2: Server 端集成

### Task 6: 修改 HttpRequestHeader 添加 fast-path 支持

**Files:**
- Modify: `galay-http/protoc/http/HttpHeader.h:85-130`
- Modify: `galay-http/protoc/http/HttpRequest.cc`

**Step 1: 在 HttpRequestHeader 添加成员变量**

在 `HttpRequestHeader` 类的 private 区域添加：

```cpp
private:
    // ... 现有成员 ...
    CommonHeaderIndex m_currentCommonHeaderIdx = CommonHeaderIndex::NotCommon;
```

**Step 2: 修改 parseChar 中的 HeaderKey 状态**

找到 `HttpRequestHeader::parseChar()` 中的 `case RequestParseState::HeaderKey:`，修改为：

```cpp
case RequestParseState::HeaderKey:
    if (c == ':') {
        if (m_parseHeaderKey.size() > 256) {
            return kBadRequest;
        }
        // Server 端：尝试匹配常见 header
        if (m_headerPairs.mode() == HeaderPair::Mode::ServerSide) {
            m_currentCommonHeaderIdx = matchCommonHeader(m_parseHeaderKey);
        }
        m_parseState = RequestParseState::HeaderColon;
    } else if (c == '\r' || c == '\n') {
        return kBadRequest;
    } else {
        if (m_parseHeaderKey.size() >= 256) {
            return kBadRequest;
        }
        // Server 端：边解析边转小写
        if (m_headerPairs.mode() == HeaderPair::Mode::ServerSide) {
            m_parseHeaderKey += toLowerAsciiChar(c);
        } else {
            m_parseHeaderKey += normalizeHeaderKeyChar(c, m_headerPairs.mode());
        }
    }
    break;
```

**Step 3: 编译验证**

Run: `cd build && make -j8`
Expected: 编译成功

**Step 4: Commit**

```bash
git add galay-http/protoc/http/HttpHeader.h galay-http/protoc/http/HttpRequest.cc
git commit -m "feat(http): HttpRequestHeader 添加 fast-path 匹配逻辑"
```

---

### Task 7: 修改 commitParsedHeaderPair 使用 fast-path

**Files:**
- Modify: `galay-http/protoc/http/HttpRequest.cc`

**Step 1: 找到 commitParsedHeaderPair 方法并修改**

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
            m_headerPairs.addNormalizedHeaderPair(
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

**Step 2: 编译验证**

Run: `cd build && make -j8`
Expected: 编译成功

**Step 3: Commit**

```bash
git add galay-http/protoc/http/HttpRequest.cc
git commit -m "feat(http): commitParsedHeaderPair 使用 fast-path 存储"
```

---

### Task 8: 修改 HeaderPair::getValue 支持 fast-path 查询

**Files:**
- Modify: `galay-http/protoc/http/HttpHeader.cc`

**Step 1: 修改 getValue 方法**

找到 `HeaderPair::getValue()` 方法，修改为：

```cpp
std::string HeaderPair::getValue(const std::string& key) const {
    if (m_mode == Mode::ServerSide) {
        // 先尝试 fast path
        CommonHeaderIndex idx = matchCommonHeader(key);
        if (idx != CommonHeaderIndex::NotCommon) {
            auto view = getCommonHeader(idx);
            return std::string(view);
        }
    }

    // Slow path: 查 map
    auto it = findHeaderPairIter(m_mode, const_cast<std::map<std::string, std::string>&>(m_headerPairs), key);
    if (it != m_headerPairs.end()) {
        return it->second;
    }
    return "";
}
```

**Step 2: 同样修改 getValuePtr 方法**

```cpp
const std::string* HeaderPair::getValuePtr(const std::string& key) const {
    if (m_mode == Mode::ServerSide) {
        // Fast path 不支持返回指针（因为返回的是 string_view）
        // 只能走 slow path
    }

    auto it = findHeaderPairIter(m_mode, const_cast<std::map<std::string, std::string>&>(m_headerPairs), key);
    if (it != m_headerPairs.end()) {
        return &it->second;
    }
    return nullptr;
}
```

**Step 3: 修改 hasKey 方法**

```cpp
bool HeaderPair::hasKey(const std::string& key) const {
    if (m_mode == Mode::ServerSide) {
        CommonHeaderIndex idx = matchCommonHeader(key);
        if (idx != CommonHeaderIndex::NotCommon) {
            return hasCommonHeader(idx);
        }
    }

    auto it = findHeaderPairIter(m_mode, const_cast<std::map<std::string, std::string>&>(m_headerPairs), key);
    return it != m_headerPairs.end();
}
```

**Step 4: 编译验证**

Run: `cd build && make -j8`
Expected: 编译成功

**Step 5: Commit**

```bash
git add galay-http/protoc/http/HttpHeader.cc
git commit -m "feat(http): HeaderPair 查询方法支持 fast-path"
```

---

### Task 9: 修改 HttpResponseHeader 同样支持 fast-path

**Files:**
- Modify: `galay-http/protoc/http/HttpHeader.h`
- Modify: `galay-http/protoc/http/HttpResponse.cc`

**Step 1: 在 HttpResponseHeader 添加成员变量**

```cpp
private:
    // ... 现有成员 ...
    CommonHeaderIndex m_currentCommonHeaderIdx = CommonHeaderIndex::NotCommon;
```

**Step 2: 修改 parseChar 中的 HeaderKey 状态**

与 Task 6 类似，修改 `HttpResponseHeader::parseChar()` 中的 `case ResponseParseState::HeaderKey:`。

**Step 3: 修改 commitParsedHeaderPair**

与 Task 7 类似。

**Step 4: 编译验证**

Run: `cd build && make -j8`
Expected: 编译成功

**Step 5: Commit**

```bash
git add galay-http/protoc/http/HttpHeader.h galay-http/protoc/http/HttpResponse.cc
git commit -m "feat(http): HttpResponseHeader 支持 fast-path"
```

---

### Task 10: 运行现有测试确保兼容性

**Files:**
- Test: `test/T1-HttpParser.cc` 等现有测试

**Step 1: 运行所有 HTTP 相关测试**

Run: `cd build && ctest -R "T1-HttpParser|T2-" -V`
Expected: 所有测试通过

**Step 2: 如果有测试失败，分析原因**

检查是否是因为：
- Mode 默认值不对
- 大小写处理不一致
- 查询逻辑有 bug

**Step 3: 修复问题并重新测试**

**Step 4: Commit（如果有修复）**

```bash
git add <修复的文件>
git commit -m "fix(http): 修复 fast-path 兼容性问题"
```

---

## Phase 3: Client 端适配

### Task 11: 确保 Client 端使用 ClientSide 模式

**Files:**
- Modify: `galay-http/kernel/http/HttpClient.h`
- Modify: `galay-http/kernel/websocket/WsClient.h`

**Step 1: 检查 HttpClient 中的 HeaderPair 初始化**

找到 `HttpClientConfig` 结构体，将 `header_mode` 从 `Canonical` 改为 `ClientSide`：

```cpp
struct HttpClientConfig {
    // ... 其他字段 ...
    HeaderPair::Mode header_mode = HeaderPair::Mode::ClientSide;
};
```

**Step 2: 同样修改 WsClient**

```cpp
struct WsClientConfig {
    // ... 其他字段 ...
    HeaderPair::Mode header_mode = HeaderPair::Mode::ClientSide;
};
```

**Step 3: 修改 Http1_1RequestBuilder**

在 `galay-http/utils/Http1_1RequestBuilder.h` 中，将默认参数从 `Canonical` 改为 `ClientSide`：

```cpp
explicit Http1_1RequestBuilder(HeaderPair::Mode mode = HeaderPair::Mode::ClientSide);

static Http1_1RequestBuilder get(const std::string& uri, HeaderPair::Mode mode = HeaderPair::Mode::ClientSide);
// ... 其他方法类似
```

**Step 4: 编译验证**

Run: `cd build && make -j8`
Expected: 编译成功

**Step 5: Commit**

```bash
git add galay-http/kernel/http/HttpClient.h galay-http/kernel/websocket/WsClient.h galay-http/utils/Http1_1RequestBuilder.h
git commit -m "feat(http): Client 端使用 ClientSide 模式保留原始大小写"
```

---

### Task 12: 编写 Client 端大小写保留测试

**Files:**
- Create: `test/T40-ClientHeaderCase.cc`

**Step 1: 创建测试文件**

```cpp
#include "../galay-http/protoc/http/HttpHeader.h"
#include <gtest/gtest.h>

using namespace galay::http;

TEST(ClientHeaderCase, PreservesOriginalCase) {
    HeaderPair headers(HeaderPair::Mode::ClientSide);

    headers.addHeaderPair("Content-Type", "application/json");
    headers.addHeaderPair("X-Custom-Header", "value");
    headers.addHeaderPair("Authorization", "Bearer token");

    // 验证原始大小写保留
    std::vector<std::pair<std::string, std::string>> result;
    headers.forEachHeader([&](std::string_view k, std::string_view v) {
        result.emplace_back(k, v);
    });

    EXPECT_EQ(result.size(), 3);

    // 查找并验证大小写
    bool found_content_type = false;
    bool found_custom = false;
    bool found_auth = false;

    for (const auto& [k, v] : result) {
        if (k == "Content-Type") found_content_type = true;
        if (k == "X-Custom-Header") found_custom = true;
        if (k == "Authorization") found_auth = true;
    }

    EXPECT_TRUE(found_content_type);
    EXPECT_TRUE(found_custom);
    EXPECT_TRUE(found_auth);
}

TEST(ClientHeaderCase, NoFastPathUsed) {
    HeaderPair headers(HeaderPair::Mode::ClientSide);

    // Client 端不应该使用 fast-path
    headers.addHeaderPair("Host", "example.com");
    headers.addHeaderPair("Content-Length", "100");

    // 验证存储在 map 中，不在 common headers 中
    EXPECT_FALSE(headers.hasCommonHeader(CommonHeaderIndex::Host));
    EXPECT_FALSE(headers.hasCommonHeader(CommonHeaderIndex::ContentLength));

    // 但可以通过 getValue 查询到
    EXPECT_EQ(headers.getValue("Host"), "example.com");
    EXPECT_EQ(headers.getValue("Content-Length"), "100");
}
```

**Step 2: 添加到 CMakeLists.txt**

```cmake
add_executable(T40-ClientHeaderCase T40-ClientHeaderCase.cc)
target_link_libraries(T40-ClientHeaderCase galay-http GTest::gtest_main)
add_test(NAME T40-ClientHeaderCase COMMAND T40-ClientHeaderCase)
```

**Step 3: 运行测试**

Run: `cd build && make T40-ClientHeaderCase && ./test/T40-ClientHeaderCase`
Expected: 所有测试通过

**Step 4: Commit**

```bash
git add test/T40-ClientHeaderCase.cc test/CMakeLists.txt
git commit -m "test(http): 添加 Client 端大小写保留测试"
```

---

## Phase 4: 性能验证

### Task 13: 编写性能基准测试

**Files:**
- Create: `benchmark/B15-HeaderParsing.cc`

**Step 1: 创建 benchmark 文件**

```cpp
#include "../galay-http/protoc/http/HttpHeader.h"
#include <benchmark/benchmark.h>

using namespace galay::http;

static void BM_ParseCommonHeaders(benchmark::State& state) {
    const char* request =
        "GET /index.html HTTP/1.1\r\n"
        "host: example.com\r\n"
        "user-agent: Mozilla/5.0\r\n"
        "accept: text/html\r\n"
        "accept-encoding: gzip, deflate\r\n"
        "connection: keep-alive\r\n"
        "content-length: 0\r\n"
        "\r\n";

    for (auto _ : state) {
        HttpRequestHeader header;
        auto [err, consumed] = header.fromString(request);
        benchmark::DoNotOptimize(header);
        benchmark::DoNotOptimize(err);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseCommonHeaders);

static void BM_ParseRareHeaders(benchmark::State& state) {
    const char* request =
        "GET /index.html HTTP/1.1\r\n"
        "x-custom-1: value1\r\n"
        "x-custom-2: value2\r\n"
        "x-custom-3: value3\r\n"
        "x-forwarded-for: 1.2.3.4\r\n"
        "x-request-id: abc123\r\n"
        "\r\n";

    for (auto _ : state) {
        HttpRequestHeader header;
        auto [err, consumed] = header.fromString(request);
        benchmark::DoNotOptimize(header);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseRareHeaders);

static void BM_ParseMixedHeaders(benchmark::State& state) {
    const char* request =
        "GET /api/data HTTP/1.1\r\n"
        "host: api.example.com\r\n"
        "content-type: application/json\r\n"
        "content-length: 256\r\n"
        "authorization: Bearer token123\r\n"
        "x-api-key: secret\r\n"
        "x-request-id: req-456\r\n"
        "user-agent: CustomClient/1.0\r\n"
        "\r\n";

    for (auto _ : state) {
        HttpRequestHeader header;
        auto [err, consumed] = header.fromString(request);
        benchmark::DoNotOptimize(header);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseMixedHeaders);

static void BM_HeaderLookup_Common(benchmark::State& state) {
    HttpRequestHeader header;
    const char* request =
        "GET / HTTP/1.1\r\n"
        "host: example.com\r\n"
        "content-length: 100\r\n"
        "user-agent: test\r\n"
        "\r\n";
    header.fromString(request);

    for (auto _ : state) {
        auto host = header.headerPairs().getValue("host");
        auto length = header.headerPairs().getValue("content-length");
        auto ua = header.headerPairs().getValue("user-agent");
        benchmark::DoNotOptimize(host);
        benchmark::DoNotOptimize(length);
        benchmark::DoNotOptimize(ua);
    }

    state.SetItemsProcessed(state.iterations() * 3);
}
BENCHMARK(BM_HeaderLookup_Common);

BENCHMARK_MAIN();
```

**Step 2: 添加到 CMakeLists.txt**

```cmake
add_executable(B15-HeaderParsing B15-HeaderParsing.cc)
target_link_libraries(B15-HeaderParsing galay-http benchmark::benchmark)
```

**Step 3: 运行 benchmark**

Run: `cd build && make B15-HeaderParsing && ./benchmark/B15-HeaderParsing`
Expected: 输出性能数据

**Step 4: 记录基准数据**

保存输出到文件：
```bash
./benchmark/B15-HeaderParsing --benchmark_out=header_parsing_optimized.json --benchmark_out_format=json
```

**Step 5: Commit**

```bash
git add benchmark/B15-HeaderParsing.cc benchmark/CMakeLists.txt
git commit -m "perf(http): 添加 header parsing 性能基准测试"
```

---

### Task 14: 运行完整 HTTP server benchmark 对比

**Files:**
- Run: `benchmark/B1-HttpServer.cc`

**Step 1: 编译优化版本**

Run: `cd build && make B1-HttpServer`

**Step 2: 运行 benchmark 并记录结果**

```bash
./benchmark/B1-HttpServer > results_optimized.txt
```

**Step 3: 对比优化前后的 RPS**

查看 `benchmark/results/20260305-093125-galay-go-rust-http-proto-compare/metrics.csv` 中的基准数据：
- 优化前: 116,136 RPS

预期优化后: 135,000+ RPS (提升 15-20%)

**Step 4: 如果性能提升不达预期，使用 perf 分析**

```bash
perf record -g ./benchmark/B1-HttpServer
perf script | stackcollapse-perf.pl | flamegraph.pl > optimized.svg
```

对比火焰图，查看 `toLower` 和 `map::insert` 的占比是否下降。

---

### Task 15: 更新文档和提交

**Files:**
- Modify: `docs/plans/2026-03-05-header-parsing-optimization-design.md`
- Create: `docs/performance/header-parsing-optimization-results.md`

**Step 1: 记录性能测试结果**

创建结果文档：

```markdown
# Header Parsing 优化结果

## 优化前后对比

### Micro Benchmark

| 测试场景 | 优化前 (ns/op) | 优化后 (ns/op) | 提升 |
|---------|---------------|---------------|------|
| 常见 headers | XXX | YYY | ZZ% |
| 罕见 headers | XXX | YYY | ZZ% |
| 混合 headers | XXX | YYY | ZZ% |
| Header 查询 | XXX | YYY | ZZ% |

### 端到端 Benchmark

| 协议 | 优化前 RPS | 优化后 RPS | 提升 |
|------|-----------|-----------|------|
| HTTP/1.1 | 116,136 | XXX,XXX | XX% |

## 火焰图分析

- `toLower` 调用占比：从 X% 降到 Y%
- `map::insert` 占比：从 X% 降到 Y%
- `matchCommonHeader` 新增占比：Z%

## 结论

✅ 达成目标：HTTP/1.1 parsing 提升 15-20%
✅ 减少 toLower 调用 80%+
✅ 常见 header 查询 O(1)
```

**Step 2: 更新设计文档状态**

在设计文档顶部修改：
```markdown
**状态**: 已实现 ✅
**实现日期**: 2026-03-XX
**性能提升**: XX%
```

**Step 3: Commit**

```bash
git add docs/
git commit -m "docs(http): 记录 header parsing 优化结果"
```

---

## 验收清单

### 功能正确性
- [ ] 所有单元测试通过 (T39, T40)
- [ ] 现有测试通过 (T1-HttpParser 等)
- [ ] Server 端统一小写存储和查询
- [ ] Client 端保留原始大小写
- [ ] 重复 header 正确合并
- [ ] 边界保护生效

### 性能提升
- [ ] HTTP/1.1 parsing 提升 15-20%
- [ ] 常见 header 查询 O(1)
- [ ] 减少 toLower 调用 80%+
- [ ] 火焰图确认优化生效

### 兼容性
- [ ] 现有 API 不变
- [ ] Client 端兼容老旧服务器
- [ ] 与 HTTP/2 统一（都用小写）

---

## 执行说明

本实现计划包含 15 个任务，预计 6-9 天完成。

**推荐执行方式**：
1. **Subagent-Driven (当前会话)** - 每个任务派发新 subagent，任务间 review
2. **Parallel Session (独立会话)** - 在新会话中使用 executing-plans skill 批量执行

选择哪种方式？
