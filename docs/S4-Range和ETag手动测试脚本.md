# S4-Range和ETag手动测试脚本文档

## 脚本概述

`S4-TestRangeEtagManual.sh` 是一个交互式手动测试脚本，用于验证 HTTP Range 请求和 ETag 缓存功能的正确性。该脚本使用 `curl` 命令执行一系列精心设计的测试用例，帮助开发者直观地观察和验证服务器对 HTTP Range 和 ETag 的支持情况。

## 功能说明

### 主要功能

1. **完整文件请求测试**：获取完整文件并提取 ETag
2. **条件请求测试**：使用 If-None-Match 测试 304 响应
3. **单范围请求测试**：测试指定字节范围的部分内容请求
4. **后缀范围请求测试**：测试获取文件末尾指定字节数
5. **前缀范围请求测试**：测试从指定位置到文件末尾的请求
6. **多范围请求测试**：测试同时请求多个不连续的字节范围
7. **If-Range 条件测试**：测试 ETag 匹配和不匹配的条件范围请求
8. **无效范围测试**：测试超出文件大小的范围请求（416 响应）

### 核心特性

- **交互式执行**：每个测试前显示说明，等待用户确认
- **详细输出**：显示完整的 HTTP 请求和响应头
- **自动 ETag 提取**：自动从响应中提取 ETag 用于后续测试
- **清晰标注**：每个测试都有编号和预期结果说明
- **循序渐进**：测试用例从简单到复杂，便于理解

## 使用方法

### 基本语法

```bash
./scripts/S4-TestRangeEtagManual.sh
```

### 前置条件

1. **启动测试服务器**：
   ```bash
   cd build/test
   ./test_static_file_server
   ```

2. **准备测试文件**：
   确保 `static/test.txt` 文件存在且内容足够大（建议 > 1KB）

3. **安装 curl**：
   ```bash
   # macOS（通常已预装）
   brew install curl

   # Linux
   sudo apt-get install curl
   ```

## 使用示例

### 1. 基本使用流程

```bash
# 步骤 1: 启动测试服务器
cd build/test
./test_static_file_server

# 步骤 2: 打开新终端，运行测试脚本
cd /path/to/project
./scripts/S4-TestRangeEtagManual.sh

# 步骤 3: 按照提示操作
# 脚本会提示启动服务器，按 Enter 继续
# 然后依次执行 9 个测试用例
```

### 2. 测试输出示例

#### Test 1: 获取完整文件并查看 ETag

```
=== Test 1: 获取完整文件并查看 ETag ===
命令: curl -i http://localhost:8080/static/test.txt

HTTP/1.1 200 OK
Content-Type: text/plain
Content-Length: 1024
ETag: "abc123def456"
Last-Modified: Wed, 29 Jan 2026 12:00:00 GMT

[文件内容...]

提取的 ETag: "abc123def456"
✓ Test 1 完成
```

#### Test 2: 使用 If-None-Match 测试 304 响应

```
=== Test 2: 使用 If-None-Match 测试 304 响应 ===
命令: curl -i -H "If-None-Match: "abc123def456"" http://localhost:8080/static/test.txt

HTTP/1.1 304 Not Modified
ETag: "abc123def456"
Last-Modified: Wed, 29 Jan 2026 12:00:00 GMT

✓ Test 2 完成（应该看到 304 Not Modified）
```

#### Test 3: 单范围请求（前 100 字节）

```
=== Test 3: 单范围请求（前 100 字节）===
命令: curl -i -H "Range: bytes=0-99" http://localhost:8080/static/test.txt

HTTP/1.1 206 Partial Content
Content-Type: text/plain
Content-Range: bytes 0-99/1024
Content-Length: 100
ETag: "abc123def456"

[前 100 字节的内容...]

✓ Test 3 完成（应该看到 206 Partial Content 和 Content-Range: bytes 0-99/...）
```

## 测试用例详解

### Test 1: 获取完整文件并查看 ETag

**目的**：验证服务器能正确返回完整文件和 ETag

**请求**：
```bash
curl -i http://localhost:8080/static/test.txt
```

**预期响应**：
- 状态码：`200 OK`
- 响应头包含：`ETag: "..."`
- 响应头包含：`Content-Length: ...`
- 响应体：完整文件内容

**验证点**：
- ETag 格式正确（通常是带引号的字符串）
- Content-Length 与实际文件大小一致
- 文件内容完整

### Test 2: 使用 If-None-Match 测试 304 响应

**目的**：验证服务器能正确处理条件请求，返回 304 响应

**请求**：
```bash
curl -i -H "If-None-Match: \"abc123def456\"" http://localhost:8080/static/test.txt
```

**预期响应**：
- 状态码：`304 Not Modified`
- 响应头包含：`ETag: "..."`
- 响应体：空（无内容传输）

**验证点**：
- 状态码必须是 304
- ETag 与请求中的 If-None-Match 匹配
- 无响应体，节省带宽

**应用场景**：
- 浏览器缓存验证
- CDN 缓存更新检查
- 客户端缓存策略

### Test 3: 单范围请求（前 100 字节）

**目的**：验证服务器能正确处理单个字节范围请求

**请求**：
```bash
curl -i -H "Range: bytes=0-99" http://localhost:8080/static/test.txt
```

**预期响应**：
- 状态码：`206 Partial Content`
- 响应头包含：`Content-Range: bytes 0-99/1024`
- 响应头包含：`Content-Length: 100`
- 响应体：文件的前 100 字节

**验证点**：
- 状态码必须是 206
- Content-Range 格式正确：`bytes start-end/total`
- Content-Length 等于请求的字节数（100）
- 响应内容与文件前 100 字节一致

**应用场景**：
- 视频播放器拖动进度条
- 大文件分段下载
- 断点续传

### Test 4: 后缀范围请求（最后 100 字节）

**目的**：验证服务器能正确处理后缀范围请求

**请求**：
```bash
curl -i -H "Range: bytes=-100" http://localhost:8080/static/test.txt
```

**预期响应**：
- 状态码：`206 Partial Content`
- 响应头包含：`Content-Range: bytes 924-1023/1024`（假设文件 1024 字节）
- 响应头包含：`Content-Length: 100`
- 响应体：文件的最后 100 字节

**验证点**：
- 正确计算起始位置（文件大小 - 100）
- Content-Range 范围正确
- 返回的是文件末尾内容

**应用场景**：
- 读取文件尾部日志
- 获取文件签名信息
- 检查文件完整性

### Test 5: 前缀范围请求（从 500 字节到末尾）

**目的**：验证服务器能正确处理前缀范围请求

**请求**：
```bash
curl -i -H "Range: bytes=500-" http://localhost:8080/static/test.txt
```

**预期响应**：
- 状态码：`206 Partial Content`
- 响应头包含：`Content-Range: bytes 500-1023/1024`
- 响应头包含：`Content-Length: 524`
- 响应体：从第 500 字节到文件末尾

**验证点**：
- 正确计算结束位置（文件大小 - 1）
- Content-Length 等于剩余字节数
- 返回的是指定位置到末尾的内容

**应用场景**：
- 断点续传
- 跳过文件头部
- 流式读取文件后半部分

### Test 6: 多范围请求

**目的**：验证服务器能正确处理多个不连续的字节范围

**请求**：
```bash
curl -i -H "Range: bytes=0-49,100-149,200-249" http://localhost:8080/static/test.txt
```

**预期响应**：
- 状态码：`206 Partial Content`
- 响应头包含：`Content-Type: multipart/byteranges; boundary=...`
- 响应体：multipart 格式，包含 3 个部分

**响应体格式**：
```
--BOUNDARY
Content-Type: text/plain
Content-Range: bytes 0-49/1024

[字节 0-49 的内容]
--BOUNDARY
Content-Type: text/plain
Content-Range: bytes 100-149/1024

[字节 100-149 的内容]
--BOUNDARY
Content-Type: text/plain
Content-Range: bytes 200-249/1024

[字节 200-249 的内容]
--BOUNDARY--
```

**验证点**：
- Content-Type 必须是 multipart/byteranges
- 每个部分都有独立的 Content-Range
- 边界分隔符正确
- 各部分内容与文件对应位置一致

**应用场景**：
- 稀疏文件读取
- 多段视频预加载
- 分布式文件系统

### Test 7: If-Range 条件请求（ETag 匹配）

**目的**：验证 If-Range 在 ETag 匹配时返回部分内容

**请求**：
```bash
curl -i -H "Range: bytes=0-99" -H "If-Range: \"abc123def456\"" http://localhost:8080/static/test.txt
```

**预期响应**：
- 状态码：`206 Partial Content`
- 响应头包含：`Content-Range: bytes 0-99/1024`
- 响应体：前 100 字节

**验证点**：
- ETag 匹配时，返回 206 和部分内容
- 行为与普通 Range 请求一致

**应用场景**：
- 断点续传时验证文件未变化
- 避免下载已修改的文件片段

### Test 8: If-Range 条件请求（ETag 不匹配）

**目的**：验证 If-Range 在 ETag 不匹配时返回完整文件

**请求**：
```bash
curl -i -H "Range: bytes=0-99" -H "If-Range: \"wrong-etag\"" http://localhost:8080/static/test.txt
```

**预期响应**：
- 状态码：`200 OK`
- 响应头包含：`Content-Length: 1024`（完整文件大小）
- 响应体：完整文件内容

**验证点**：
- ETag 不匹配时，忽略 Range 请求
- 返回 200 和完整文件
- 确保客户端获取最新版本

**应用场景**：
- 文件已更新，重新下载完整文件
- 避免使用过期的部分内容

### Test 9: 无效范围请求（416 响应）

**目的**：验证服务器能正确处理超出文件大小的范围请求

**请求**：
```bash
curl -i -H "Range: bytes=99999-999999" http://localhost:8080/static/test.txt
```

**预期响应**：
- 状态码：`416 Range Not Satisfiable`
- 响应头包含：`Content-Range: bytes */1024`
- 响应体：空或错误信息

**验证点**：
- 状态码必须是 416
- Content-Range 显示文件总大小
- 不返回任何文件内容

**应用场景**：
- 客户端请求错误处理
- 防止无效的范围请求
- 提供明确的错误信息

## HTTP Range 和 ETag 规范

### Range 请求格式

#### 1. 单范围请求

```
Range: bytes=start-end
```

**示例**：
- `Range: bytes=0-99`：请求前 100 字节（0-99）
- `Range: bytes=100-199`：请求第 101-200 字节

#### 2. 后缀范围请求

```
Range: bytes=-count
```

**示例**：
- `Range: bytes=-100`：请求最后 100 字节
- `Range: bytes=-1`：请求最后 1 字节

#### 3. 前缀范围请求

```
Range: bytes=start-
```

**示例**：
- `Range: bytes=500-`：从第 500 字节到文件末尾
- `Range: bytes=0-`：整个文件（等同于不带 Range）

#### 4. 多范围请求

```
Range: bytes=range1,range2,range3
```

**示例**：
- `Range: bytes=0-49,100-149`：请求两个不连续的范围
- `Range: bytes=0-99,200-299,400-499`：请求三个范围

### Range 响应格式

#### 1. 单范围响应（206）

```
HTTP/1.1 206 Partial Content
Content-Type: text/plain
Content-Range: bytes 0-99/1024
Content-Length: 100

[100 字节的内容]
```

#### 2. 多范围响应（206）

```
HTTP/1.1 206 Partial Content
Content-Type: multipart/byteranges; boundary=BOUNDARY

--BOUNDARY
Content-Type: text/plain
Content-Range: bytes 0-49/1024

[50 字节的内容]
--BOUNDARY
Content-Type: text/plain
Content-Range: bytes 100-149/1024

[50 字节的内容]
--BOUNDARY--
```

#### 3. 范围不满足响应（416）

```
HTTP/1.1 416 Range Not Satisfiable
Content-Range: bytes */1024

[可选的错误信息]
```

### ETag 机制

#### 1. ETag 生成

**强 ETag**：
```
ETag: "abc123def456"
```
- 内容完全相同才匹配
- 适用于精确缓存验证

**弱 ETag**：
```
ETag: W/"abc123def456"
```
- 语义等价即可匹配
- 适用于压缩内容等场景

#### 2. 条件请求头

**If-None-Match**：
```
If-None-Match: "abc123def456"
```
- ETag 匹配时返回 304
- 用于缓存验证

**If-Match**：
```
If-Match: "abc123def456"
```
- ETag 不匹配时返回 412
- 用于防止并发修改

**If-Range**：
```
If-Range: "abc123def456"
Range: bytes=0-99
```
- ETag 匹配时返回 206（部分内容）
- ETag 不匹配时返回 200（完整内容）

## 工作流程

### 执行流程图

```
开始
  ↓
显示脚本说明
  ↓
提示启动服务器
  ↓
等待用户按 Enter
  ↓
Test 1: 获取完整文件和 ETag
  ↓
提取 ETag
  ↓
Test 2: If-None-Match 测试（304）
  ↓
Test 3: 单范围请求（前 100 字节）
  ↓
Test 4: 后缀范围请求（最后 100 字节）
  ↓
Test 5: 前缀范围请求（从 500 字节开始）
  ↓
Test 6: 多范围请求
  ↓
Test 7: If-Range 匹配测试
  ↓
Test 8: If-Range 不匹配测试
  ↓
Test 9: 无效范围测试（416）
  ↓
显示完成信息
  ↓
结束
```

## 错误处理

### 常见错误及解决方案

#### 1. 服务器未启动

**现象**：
```
curl: (7) Failed to connect to localhost port 8080: Connection refused
```

**解决方案**：
```bash
# 启动测试服务器
cd build/test
./test_static_file_server
```

#### 2. 测试文件不存在

**现象**：
```
HTTP/1.1 404 Not Found
```

**解决方案**：
```bash
# 检查测试文件
ls -la build/test/static/test.txt

# 如果不存在，创建测试文件
cd build/test/static
dd if=/dev/urandom of=test.txt bs=1024 count=1
```

#### 3. ETag 提取失败

**现象**：
```
提取的 ETag:
```

**解决方案**：
- 检查服务器是否正确返回 ETag 头
- 确认 grep 和 cut 命令可用
- 手动查看响应头，确认 ETag 格式

#### 4. Range 请求返回 200

**现象**：
服务器返回 200 OK 而不是 206 Partial Content

**可能原因**：
- 服务器不支持 Range 请求
- Range 头格式错误
- 文件太小（某些服务器对小文件不支持 Range）

**解决方案**：
- 检查服务器 Range 支持
- 验证 Range 头格式
- 使用较大的测试文件（> 1KB）

## 注意事项

### 使用建议

1. **服务器准备**：
   - 确保服务器支持 Range 和 ETag
   - 测试文件大小建议 > 1KB
   - 服务器日志级别设置为 DEBUG 便于调试

2. **测试环境**：
   - 使用本地服务器（localhost）
   - 避免通过代理或 CDN
   - 关闭浏览器缓存影响

3. **结果验证**：
   - 仔细检查状态码
   - 验证 Content-Range 格式
   - 对比响应内容与文件内容

4. **调试技巧**：
   - 使用 `-v` 参数查看详细信息：`curl -v ...`
   - 保存响应到文件：`curl ... > response.txt`
   - 使用 hexdump 验证二进制内容：`hexdump -C response.txt`

### 最佳实践

1. **逐步测试**：
   ```bash
   # 先测试基本功能
   curl -i http://localhost:8080/static/test.txt

   # 再测试 Range 功能
   curl -i -H "Range: bytes=0-99" http://localhost:8080/static/test.txt

   # 最后测试复杂场景
   curl -i -H "Range: bytes=0-49,100-149" http://localhost:8080/static/test.txt
   ```

2. **验证内容正确性**：
   ```bash
   # 获取完整文件
   curl -s http://localhost:8080/static/test.txt > full.txt

   # 获取部分内容
   curl -s -H "Range: bytes=0-99" http://localhost:8080/static/test.txt > partial.txt

   # 对比内容
   head -c 100 full.txt > expected.txt
   diff expected.txt partial.txt
   ```

3. **性能测试**：
   ```bash
   # 测试 Range 请求性能
   time curl -s -H "Range: bytes=0-99" http://localhost:8080/static/test.txt > /dev/null

   # 测试 304 响应性能
   ETAG=$(curl -s -I http://localhost:8080/static/test.txt | grep ETag | cut -d' ' -f2)
   time curl -s -H "If-None-Match: $ETAG" http://localhost:8080/static/test.txt > /dev/null
   ```

## 脚本实现细节

### 关键变量

```bash
SERVER="http://localhost:8080"      # 服务器地址
TEST_FILE="/static/test.txt"        # 测试文件路径
TEST_URL="${SERVER}${TEST_FILE}"    # 完整测试 URL
ETAG=""                              # 提取的 ETag 值
```

### ETag 提取逻辑

```bash
# 发送请求并保存响应
RESPONSE=$(curl -i -s "$TEST_URL")

# 提取 ETag（忽略大小写，去除回车符）
ETAG=$(echo "$RESPONSE" | grep -i "ETag:" | cut -d' ' -f2 | tr -d '\r')
```

**说明**：
- `grep -i "ETag:"`：忽略大小写查找 ETag 头
- `cut -d' ' -f2`：以空格分隔，取第二个字段
- `tr -d '\r'`：删除 Windows 风格的回车符

### 测试标记

每个测试都有清晰的标记：

```bash
echo "=== Test N: 测试描述 ==="
echo "命令: curl ..."
echo ""
# 执行测试
echo ""
echo "✓ Test N 完成（预期结果说明）"
echo ""
```

## 扩展功能

### 自动化验证

可以添加自动验证逻辑：

```bash
# 验证状态码
STATUS=$(curl -s -o /dev/null -w "%{http_code}" -H "Range: bytes=0-99" "$TEST_URL")
if [ "$STATUS" = "206" ]; then
    echo "✓ 状态码正确"
else
    echo "✗ 状态码错误: $STATUS (期望 206)"
fi
```

### 批量测试

可以将测试用例参数化：

```bash
# 测试用例数组
declare -a TESTS=(
    "0-99:206"
    "-100:206"
    "500-:206"
    "99999-999999:416"
)

# 循环执行
for test in "${TESTS[@]}"; do
    range="${test%%:*}"
    expected="${test##*:}"
    # 执行测试并验证
done
```

### 生成测试报告

可以将结果输出为结构化格式：

```bash
# 输出为 JSON
{
    "test_1": {
        "status": "pass",
        "status_code": 200,
        "etag": "abc123"
    },
    "test_2": {
        "status": "pass",
        "status_code": 304
    }
}
```

## 相关脚本

- **S1-Run.sh**：运行测试和示例的通用脚本
- **S2-Check.sh**：验证测试结果和压测指标
- **S3-BenchmarkStaticFiles.sh**：静态文件服务压测
- **S5-TestRangeEtagStress.sh**：HTTP Range 和 ETag 压力测试

## 技术规格

| 项目 | 说明 |
|------|------|
| Shell 版本 | Bash 3.2+ |
| 依赖工具 | curl, grep, cut, tr |
| 支持平台 | Linux, macOS |
| 脚本位置 | `scripts/S4-TestRangeEtagManual.sh` |
| 测试用例数 | 9 个 |
| 交互模式 | 是 |

## 参考资料

### HTTP 规范

- **RFC 7233**：HTTP/1.1 Range Requests
- **RFC 7232**：HTTP/1.1 Conditional Requests
- **RFC 2616**：HTTP/1.1 (已废弃，但仍有参考价值)

### 相关概念

- **206 Partial Content**：部分内容响应
- **304 Not Modified**：未修改响应
- **416 Range Not Satisfiable**：范围不满足响应
- **ETag**：实体标签，用于缓存验证
- **Content-Range**：内容范围头，指示返回的字节范围

---

**文档版本**：v1.0
**创建日期**：2026-01-29
**维护团队**：galay-http 开发团队
