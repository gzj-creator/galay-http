# [历史档案] HTTP/2 性能瓶颈分析报告

> 档案状态：本页保存一次历史性能分析快照，其中的 RPS、火焰图样本与对比结论未在 2026-03-10 文档修复中重新复现。
> 使用规则：本页仅作调优背景材料；当前 benchmark target、运行命令和“是否为现行结论”的判断，以 `docs/05-性能测试.md` 为准。

## 历史快照

### 当时采样的 RPS 对比
- **Galay H2**: 56,700 RPS (落后 Go 28.3%, 落后 Rust 29.7%)
- **Go H2**: 79,080 RPS
- **Rust H2**: 80,610 RPS
- **排名**: #3/3 ❌

### 火焰图样本数对比
- **Galay H2**: 44,104 samples
- **Go H2**: 15,905 samples (仅 36% 的样本数)
- **Rust H2**: 63,399 samples (仅 14.5% 的样本数)

**历史观察**: Galay 的样本数是 Go 的 2.8 倍，是 Rust 的 6.9 倍！
这说明 Galay 在相同时间内执行了更多的函数调用，存在严重的 CPU 效率问题。

---

## 历史瓶颈分析

### 1. 协程调度开销过高 (最严重)

**问题**: 91% 的样本在等待队列和调度器
```
semaphore_timedwait_trap:        39,866 samples (90.4%)
Scheduler::resume:                4,251 samples (9.6%)
processPendingCoroutines:         2,660 samples (6.0%)
```

**对比**:
- Galay: 90.4% 时间在等待/调度
- Go/Rust: 样本数少得多，说明调度效率高

**根因**:
- 过度使用协程（每个 stream 都是独立协程）
- 协程切换开销大（moodycamel 队列 + semaphore）
- 大量空闲等待（semaphore_timedwait_trap）

---

### 2. TLS 加密开销

**H2 vs H2C 对比**:
- H2 (TLS): 56,700 RPS
- H2C (TCP): 80,134 RPS (+41% 性能)

**TLS 热点**:
```
SSL_write:                        336 samples
ssl3_write_bytes:                 326 samples
tls_write_records:                285 samples
tls13_cipher:                     275 samples
SSL_read:                         203 samples
```

**问题**:
- TLS 握手和加密占用大量 CPU
- 每次 send/recv 都要经过 SSL 层
- 没有充分利用 TLS session 复用

---

### 3. Frame 解析和 HPACK 编解码

**热点函数**:
```
parseFrameFromBuffer:             100 samples
HpackDecoder::decode:              41 samples
HpackEncoder::encode:              34 samples
Http2FrameParser::parseFrame:     22 samples
HpackEncoder::encodeField:         18 samples
```

**问题**:
- `vector<Http2HeaderField>::__push_back_slow_path`: 22 samples
  - 动态扩容导致内存重新分配
  - 应该预分配容量（reserve）

- HPACK 编解码效率低
  - 每个 header 都要查表
  - 没有缓存常见 header 的编码结果

---

### 4. 内存分配开销

**热点**:
```
free_tiny:                        232 samples
operator new:                     213 samples
```

**问题**:
- 频繁的小对象分配/释放
- Frame 对象、HeaderField vector 的动态分配
- 没有使用对象池

---

### 5. Stream 管理开销

**热点**:
```
Http2StreamManagerImpl::runHandler:     1,886 samples
Http2StreamManagerImpl::writerLoop:     1,509 samples
Http2StreamManagerImpl::readerLoop:       675 samples
Http2Stream::sendHeadersInternal:       1,087 samples
Http2Stream::sendDataInternal:            499 samples
```

**问题**:
- 每个 stream 都有独立的 reader/writer loop
- 大量协程切换开销
- Stream 之间缺乏批量处理

---

## 优化建议（按优先级）

### 🔴 优先级 1: 减少协程调度开销

**方案 A: 批量处理 Frames**
```cpp
// 当前：每个 frame 触发一次协程切换
co_await readFrame();  // 切换
processFrame();
co_await readFrame();  // 切换

// 优化：批量读取多个 frames
auto frames = co_await readFramesBatch(max_count);
for (auto& frame : frames) {
    processFrame(frame);
}
```

**预期收益**: 减少 50% 的协程切换

---

**方案 B: 合并 Reader/Writer Loop**
```cpp
// 当前：每个 stream 3 个协程（runHandler + readerLoop + writerLoop）
// 优化：使用单个协程 + 事件驱动

class Http2Stream {
    co_task<void> eventLoop() {
        while (true) {
            auto event = co_await waitEvent();  // 只等待一次
            switch (event.type) {
                case READ_READY:  handleRead(); break;
                case WRITE_READY: handleWrite(); break;
                case HANDLER:     handleRequest(); break;
            }
        }
    }
};
```

**预期收益**: 减少 66% 的协程数量

---

### 🟡 优先级 2: 优化 HPACK 编解码

**方案 A: 预分配 HeaderField Vector**
```cpp
// 当前：动态扩容导致 __push_back_slow_path
std::vector<Http2HeaderField> headers;

// 优化：预分配
std::vector<Http2HeaderField> headers;
headers.reserve(16);  // 大多数请求 < 16 个 header
```

**预期收益**: 消除 22 samples 的 slow_path

---

**方案 B: 缓存常见 Header 的 HPACK 编码**
```cpp
// 缓存常见 header 的编码结果
static const auto cached_content_type_json =
    hpackEncode(":content-type", "application/json");

// 直接使用缓存
if (content_type == "application/json") {
    output.append(cached_content_type_json);
} else {
    output.append(hpackEncode(":content-type", content_type));
}
```

**预期收益**: 减少 30% 的 HPACK 编码时间

---

**方案 C: 复用 HTTP/1.1 的 Header Fast-Path**
```cpp
// HTTP/1.1 已经有 fast-path（array + O(1) 查询）
// HTTP/2 可以直接复用这个优化

// 在 HPACK 编码时，先检查是否是常见 header
CommonHeaderIndex idx = matchCommonHeader(key);
if (idx != CommonHeaderIndex::NotCommon) {
    // 使用预编码的 HPACK 结果
    output.append(commonHeaderHpackEncoded[idx]);
}
```

**预期收益**: 与 HTTP/1.1 类似，减少 15-20% 的编码时间

---

### 🟢 优先级 3: 优化 TLS 性能

**方案 A: TLS Session 复用**
```cpp
// 启用 TLS session cache
SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER);
SSL_CTX_sess_set_cache_size(ctx, 1024);
```

**预期收益**: 减少 50% 的 TLS 握手开销

---

**方案 B: 使用 AES-NI 硬件加速**
```cpp
// 确保使用支持硬件加速的 cipher suite
SSL_CTX_set_cipher_list(ctx, "ECDHE-RSA-AES128-GCM-SHA256");
```

**预期收益**: 减少 20-30% 的加密开销

---

**方案 C: 批量 TLS 写入**
```cpp
// 当前：每个 frame 单独 SSL_write
SSL_write(ssl, frame1);
SSL_write(ssl, frame2);

// 优化：合并多个 frames 后一次写入
buffer.append(frame1);
buffer.append(frame2);
SSL_write(ssl, buffer);
```

**预期收益**: 减少 30% 的 SSL_write 调用

---

### 🟢 优先级 4: 减少内存分配

**方案 A: Frame 对象池**
```cpp
class Http2FramePool {
    std::vector<std::unique_ptr<Http2Frame>> pool;

    Http2Frame* acquire() {
        if (pool.empty()) return new Http2Frame();
        auto frame = pool.back().release();
        pool.pop_back();
        return frame;
    }

    void release(Http2Frame* frame) {
        frame->reset();
        pool.emplace_back(frame);
    }
};
```

**预期收益**: 减少 50% 的 new/delete 调用

---

## 量化预期

如果实施上述优化：

| 优化项 | 预期提升 | 累计提升 |
|--------|---------|---------|
| 减少协程调度开销 | +30-40% | 73,710 RPS |
| 优化 HPACK 编解码 | +10-15% | 84,767 RPS |
| 优化 TLS 性能 | +15-20% | 101,720 RPS |
| 减少内存分配 | +5-10% | 111,892 RPS |

**总体预期**: 从 56,700 RPS 提升到 **110,000+ RPS** (+97%)
- 超越 Go (79,080 RPS) ✅
- 超越 Rust (80,610 RPS) ✅

---

## 下一步行动

**建议优先实施**:
1. **批量处理 Frames** (投入产出比最高)
2. **预分配 HeaderField Vector** (简单且有效)
3. **复用 HTTP/1.1 Header Fast-Path** (已有基础设施)

**实施顺序**:
1. 先优化 H2C（无 TLS 干扰，更容易验证效果）
2. 再将优化应用到 H2
3. 最后优化 TLS 层

需要我帮你实现其中某个优化吗？
