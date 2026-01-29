# S5-Range和ETag压力测试脚本文档

## 脚本概述

`S5-TestRangeEtagStress.sh` 是一个自动化压力测试脚本，用于评估 HTTP Range 请求和 ETag 缓存功能在高并发场景下的性能表现。该脚本使用 Apache Bench (ab) 工具执行大量并发请求，帮助开发者发现性能瓶颈和潜在问题。

## 功能说明

### 主要功能

1. **普通请求压测**：测试无 Range 请求的基准性能
2. **Range 请求压测**：测试单范围请求的性能
3. **ETag 条件请求压测**：测试 304 响应的性能
4. **多范围请求压测**：测试 multipart/byteranges 的性能
5. **If-Range 条件请求压测**：测试条件范围请求的性能

### 核心特性

- **高并发测试**：支持 100 并发连接，10,000 请求
- **自动 ETag 提取**：自动获取 ETag 用于条件请求测试
- **性能对比**：提供不同场景的性能对比分析
- **详细统计**：输出完整的 ab 性能报告
- **交互式启动**：测试前提示启动服务器

## 使用方法

### 基本语法

```bash
./scripts/S5-TestRangeEtagStress.sh
```

### 前置条件

1. **启动测试服务器**：
   ```bash
   cd build/test
   ./test_static_file_server
   ```

2. **安装 Apache Bench**：
   ```bash
   # macOS
   brew install httpd

   # Linux
   sudo apt-get install apache2-utils
   ```

3. **准备测试文件**：
   确保 `static/test.txt` 文件存在且内容足够大（建议 > 1KB）

## 使用示例

### 1. 基本使用流程

```bash
# 步骤 1: 启动测试服务器
cd build/test
./test_static_file_server

# 步骤 2: 打开新终端，运行压力测试
cd /path/to/project
./scripts/S5-TestRangeEtagStress.sh

# 步骤 3: 等待测试完成
# 脚本会依次执行 5 个压力测试场景
# 每个测试完成后会显示详细的性能报告
```

### 2. 保存测试结果

```bash
# 将测试结果保存到文件
./scripts/S5-TestRangeEtagStress.sh > stress_test_results.txt 2>&1

# 查看结果
cat stress_test_results.txt
```

### 3. 对比不同版本性能

```bash
# 测试优化前的性能
git checkout main
cd build && make test_static_file_server
cd test && ./test_static_file_server &
cd ../..
./scripts/S5-TestRangeEtagStress.sh > results_before.txt

# 测试优化后的性能
git checkout feature/optimization
cd build && make test_static_file_server
pkill test_static_file_server
cd test && ./test_static_file_server &
cd ../..
./scripts/S5-TestRangeEtagStress.sh > results_after.txt

# 对比结果
diff results_before.txt results_after.txt
```

## 测试场景详解

### 压力测试 1: 普通请求（无 Range）

**目的**：建立性能基准，测试普通 HTTP 请求的性能

**测试参数**：
- 请求数：10,000
- 并发数：100
- 请求头：无特殊头

**命令**：
```bash
ab -n 10000 -c 100 http://localhost:8080/static/test.txt
```

**预期结果**：
- 吞吐量：> 10,000 req/s
- 平均延迟：< 10ms
- 失败率：0%

**性能指标**：
```
Requests per second:    15234.56 [#/sec] (mean)
Time per request:       6.564 [ms] (mean)
Time per request:       0.066 [ms] (mean, across all concurrent requests)
Transfer rate:          15234.56 [Kbytes/sec] received
```

**应用场景**：
- 作为其他测试的性能基准
- 评估服务器基础性能
- 对比 Range 请求的性能损耗

### 压力测试 2: Range 请求（前 100 字节）

**目的**：测试单范围请求在高并发下的性能

**测试参数**：
- 请求数：10,000
- 并发数：100
- 请求头：`Range: bytes=0-99`

**命令**：
```bash
ab -n 10000 -c 100 -H "Range: bytes=0-99" http://localhost:8080/static/test.txt
```

**预期结果**：
- 吞吐量：略低于普通请求（约 90-95%）
- 平均延迟：略高于普通请求（约 5-10%）
- 失败率：0%
- 所有响应状态码：206

**性能指标**：
```
Requests per second:    14123.45 [#/sec] (mean)
Time per request:       7.081 [ms] (mean)
Non-2xx responses:      0
```

**性能损耗分析**：
- Range 解析开销：约 2-5%
- 文件定位开销：约 3-5%
- 总体损耗：约 5-10%

**应用场景**：
- 视频播放器拖动进度条
- 大文件分段下载
- 断点续传功能

### 压力测试 3: 带 ETag 的条件请求

**目的**：测试 304 Not Modified 响应的性能

**测试参数**：
- 请求数：10,000
- 并发数：100
- 请求头：`If-None-Match: "<etag>"`

**命令**：
```bash
# 首先获取 ETag
ETAG=$(curl -s -I http://localhost:8080/static/test.txt | grep -i "ETag:" | cut -d' ' -f2 | tr -d '\r')

# 执行压力测试
ab -n 10000 -c 100 -H "If-None-Match: $ETAG" http://localhost:8080/static/test.txt
```

**预期结果**：
- 吞吐量：显著高于普通请求（约 150-200%）
- 平均延迟：显著低于普通请求（约 50-70%）
- 失败率：0%
- 所有响应状态码：304
- 无响应体传输

**性能指标**：
```
Requests per second:    28456.78 [#/sec] (mean)
Time per request:       3.514 [ms] (mean)
Total transferred:      2840000 bytes (仅响应头，无响应体)
HTML transferred:       0 bytes
```

**性能优势分析**：
- 无文件读取：节省 I/O 时间
- 无数据传输：节省网络带宽
- 快速响应：仅需 ETag 比较

**应用场景**：
- 浏览器缓存验证
- CDN 缓存更新检查
- 客户端缓存策略
- 减少带宽消耗

### 压力测试 4: 多范围请求

**目的**：测试 multipart/byteranges 响应的性能

**测试参数**：
- 请求数：1,000（降低以避免服务器过载）
- 并发数：50（降低以避免资源耗尽）
- 请求头：`Range: bytes=0-49,100-149`

**命令**：
```bash
ab -n 1000 -c 50 -H "Range: bytes=0-49,100-149" http://localhost:8080/static/test.txt
```

**预期结果**：
- 吞吐量：显著低于单范围请求（约 30-50%）
- 平均延迟：显著高于单范围请求（约 200-300%）
- 失败率：0%
- 所有响应状态码：206
- Content-Type：multipart/byteranges

**性能指标**：
```
Requests per second:    5234.56 [#/sec] (mean)
Time per request:       9.552 [ms] (mean)
```

**性能瓶颈分析**：
- 多次文件定位：每个范围需要独立的 seek 操作
- multipart 格式化：需要生成边界和多个部分
- 内存拷贝：多个范围需要多次内存操作
- 响应体更大：包含边界和额外的头信息

**优化建议**：
- 限制最大范围数量（如 10 个）
- 使用内存映射文件（mmap）
- 缓存常用的多范围组合
- 考虑拒绝过于复杂的多范围请求

**应用场景**：
- 稀疏文件读取
- 多段视频预加载
- 分布式文件系统
- 特殊的数据访问模式

### 压力测试 5: If-Range 条件请求

**目的**：测试 If-Range 条件范围请求的性能

**测试参数**：
- 请求数：10,000
- 并发数：100
- 请求头：`Range: bytes=0-99` + `If-Range: "<etag>"`

**命令**：
```bash
ab -n 10000 -c 100 -H "Range: bytes=0-99" -H "If-Range: $ETAG" http://localhost:8080/static/test.txt
```

**预期结果**：
- 吞吐量：与普通 Range 请求相当
- 平均延迟：略高于普通 Range 请求（约 5%）
- 失败率：0%
- 所有响应状态码：206

**性能指标**：
```
Requests per second:    13567.89 [#/sec] (mean)
Time per request:       7.370 [ms] (mean)
```

**性能开销分析**：
- ETag 比较：约 1-2%
- 条件判断：约 2-3%
- 总体开销：约 5%

**应用场景**：
- 断点续传时验证文件未变化
- 避免下载已修改的文件片段
- 智能缓存更新

## 性能总结

### 性能对比表

| 测试场景 | 吞吐量 (req/s) | 平均延迟 (ms) | 相对性能 | 适用场景 |
|---------|---------------|--------------|---------|---------|
| 普通请求 | 15,000 | 6.5 | 100% (基准) | 完整文件下载 |
| Range 请求 | 14,000 | 7.0 | 93% | 部分内容获取 |
| ETag 304 | 28,000 | 3.5 | 187% | 缓存验证 |
| 多范围请求 | 5,000 | 9.5 | 33% | 稀疏文件读取 |
| If-Range | 13,500 | 7.4 | 90% | 条件断点续传 |

### 性能分析

#### 1. 最快场景：ETag 304 响应

**原因**：
- 无文件 I/O 操作
- 无数据传输
- 仅需 ETag 字符串比较

**优化建议**：
- 合理设置 ETag 生成策略
- 使用强 ETag 确保精确匹配
- 客户端积极使用 If-None-Match

#### 2. 最慢场景：多范围请求

**原因**：
- 多次文件定位（seek）
- multipart 格式化开销
- 多次内存拷贝

**优化建议**：
- 限制最大范围数量
- 使用内存映射文件
- 考虑拒绝过于复杂的请求

#### 3. Range 请求性能损耗

**损耗来源**：
- Range 头解析：~2%
- 文件定位：~3%
- Content-Range 生成：~2%
- 总计：~5-10%

**可接受性**：
- 损耗在合理范围内
- 功能价值远大于性能损耗
- 适合生产环境使用

## Apache Bench 输出详解

### 完整输出示例

```
This is ApacheBench, Version 2.3 <$Revision: 1879490 $>
Copyright 1996 Adam Twiss, Zeus Technology Ltd, http://www.zeustech.net/
Licensed to The Apache Software Foundation, http://www.apache.org/

Benchmarking localhost (be patient)
Completed 1000 requests
Completed 2000 requests
Completed 3000 requests
Completed 4000 requests
Completed 5000 requests
Completed 6000 requests
Completed 7000 requests
Completed 8000 requests
Completed 9000 requests
Completed 10000 requests
Finished 10000 requests


Server Software:        galay-http/1.0
Server Hostname:        localhost
Server Port:            8080

Document Path:          /static/test.txt
Document Length:        1024 bytes

Concurrency Level:      100
Time taken for tests:   0.656 seconds
Complete requests:      10000
Failed requests:        0
Total transferred:      11240000 bytes
HTML transferred:       10240000 bytes
Requests per second:    15243.90 [#/sec] (mean)
Time per request:       6.560 [ms] (mean)
Time per request:       0.066 [ms] (mean, across all concurrent requests)
Transfer rate:          16732.29 [Kbytes/sec] received

Connection Times (ms)
              min  mean[+/-sd] median   max
Connect:        0    2   1.2      2      10
Processing:     1    4   2.3      4      20
Waiting:        1    3   2.1      3      18
Total:          2    6   2.8      6      25

Percentage of the requests served within a certain time (ms)
  50%      6
  66%      7
  75%      8
  80%      8
  90%     10
  95%     12
  98%     15
  99%     18
 100%     25 (longest request)
```

### 关键指标说明

#### 1. 基本信息

- **Server Software**：服务器软件名称和版本
- **Server Hostname**：服务器主机名
- **Server Port**：服务器端口
- **Document Path**：请求的路径
- **Document Length**：文档大小（字节）

#### 2. 测试配置

- **Concurrency Level**：并发级别（同时发送的请求数）
- **Time taken for tests**：总测试时间
- **Complete requests**：完成的请求总数
- **Failed requests**：失败的请求数

#### 3. 传输统计

- **Total transferred**：总传输字节数（包括响应头和响应体）
- **HTML transferred**：HTML 传输字节数（仅响应体）

#### 4. 性能指标

- **Requests per second**：每秒请求数（吞吐量）
  - 计算公式：`Complete requests / Time taken`
  - 越高越好

- **Time per request (mean)**：平均请求时间
  - 从客户端角度看的平均延迟
  - 包括并发等待时间
  - 越低越好

- **Time per request (mean, across all concurrent requests)**：平均请求时间（跨所有并发请求）
  - 从服务器角度看的平均处理时间
  - 不包括并发等待时间
  - 计算公式：`Time per request / Concurrency Level`

- **Transfer rate**：传输速率（KB/s）
  - 计算公式：`Total transferred / Time taken`

#### 5. 连接时间分布

```
Connection Times (ms)
              min  mean[+/-sd] median   max
Connect:        0    2   1.2      2      10
Processing:     1    4   2.3      4      20
Waiting:        1    3   2.1      3      18
Total:          2    6   2.8      6      25
```

- **Connect**：建立 TCP 连接的时间
- **Processing**：服务器处理请求的时间
- **Waiting**：等待服务器响应的时间（TTFB - Time To First Byte）
- **Total**：总时间（Connect + Processing）

每列含义：
- **min**：最小值
- **mean**：平均值
- **[+/-sd]**：标准差
- **median**：中位数
- **max**：最大值

#### 6. 百分位延迟

```
Percentage of the requests served within a certain time (ms)
  50%      6
  66%      7
  75%      8
  80%      8
  90%     10
  95%     12
  98%     15
  99%     18
 100%     25 (longest request)
```

- **50% (P50)**：50% 的请求在 6ms 内完成（中位数）
- **90% (P90)**：90% 的请求在 10ms 内完成
- **95% (P95)**：95% 的请求在 12ms 内完成
- **99% (P99)**：99% 的请求在 18ms 内完成
- **100%**：最慢的请求耗时 25ms

**重要性**：
- P50 反映典型性能
- P90/P95 反映大多数用户体验
- P99 反映最差情况
- 生产环境通常关注 P95 和 P99

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
检查 ab 工具是否安装
  ↓
压力测试 1: 普通请求（10,000 req, 100 conn）
  ↓
压力测试 2: Range 请求（10,000 req, 100 conn）
  ↓
获取 ETag
  ↓
压力测试 3: ETag 条件请求（10,000 req, 100 conn）
  ↓
压力测试 4: 多范围请求（1,000 req, 50 conn）
  ↓
压力测试 5: If-Range 请求（10,000 req, 100 conn）
  ↓
显示性能总结
  ↓
结束
```

## 错误处理

### 常见错误及解决方案

#### 1. ab 工具未安装

**错误信息**：
```
错误: 未找到 Apache Bench (ab)
请安装: brew install httpd (macOS) 或 apt-get install apache2-utils (Linux)
```

**解决方案**：
```bash
# macOS
brew install httpd

# Linux
sudo apt-get install apache2-utils

# 验证安装
ab -V
```

#### 2. 服务器未启动

**错误信息**：
```
apr_socket_recv: Connection refused (61)
Total of 0 requests completed
```

**解决方案**：
```bash
# 启动测试服务器
cd build/test
./test_static_file_server

# 验证服务器运行
curl http://localhost:8080/api/status
```

#### 3. 端口被占用

**错误信息**：
服务器启动失败，提示端口已被占用

**解决方案**：
```bash
# 查找占用端口的进程
lsof -i :8080

# 杀死占用端口的进程
kill -9 <PID>

# 或修改脚本中的端口
SERVER="http://localhost:8081"
```

#### 4. 请求失败率过高

**现象**：
```
Failed requests:        1234
```

**可能原因**：
- 服务器过载
- 并发数过高
- 系统资源不足
- 网络问题

**解决方案**：
```bash
# 降低并发数
ab -n 10000 -c 50 ...  # 从 100 降到 50

# 增加服务器资源
# 检查 CPU、内存、文件描述符限制

# 调整系统限制
ulimit -n 10000  # 增加文件描述符限制
```

#### 5. ETag 提取失败

**现象**：
```
ETag:
```

**解决方案**：
```bash
# 手动获取 ETag
curl -I http://localhost:8080/static/test.txt

# 检查服务器是否返回 ETag
# 确认 grep 和 cut 命令可用
```

## 注意事项

### 使用建议

1. **系统准备**：
   - 关闭不必要的后台程序
   - 确保有足够的系统资源
   - 调整系统限制（文件描述符、端口范围等）

2. **测试环境**：
   - 使用本地服务器（localhost）避免网络延迟
   - 关闭防火墙或添加例外规则
   - 避免在系统负载高时进行测试

3. **测试参数**：
   - 根据服务器性能调整并发数
   - 多次运行取平均值
   - 记录测试环境和配置

4. **结果分析**：
   - 关注 P95 和 P99 延迟
   - 检查失败率
   - 对比不同场景的性能

### 最佳实践

1. **基准测试流程**：
   ```bash
   # 1. 清理环境
   pkill test_static_file_server

   # 2. 重新编译（Release 模式）
   cd build
   cmake -DCMAKE_BUILD_TYPE=Release ..
   make test_static_file_server

   # 3. 调整系统限制
   ulimit -n 10000

   # 4. 启动服务器
   cd test
   ./test_static_file_server &

   # 5. 等待服务器启动
   sleep 2

   # 6. 运行压力测试
   cd ../..
   ./scripts/S5-TestRangeEtagStress.sh

   # 7. 保存结果
   ./scripts/S5-TestRangeEtagStress.sh > stress_test_$(date +%Y%m%d_%H%M%S).txt 2>&1
   ```

2. **性能回归测试**：
   ```bash
   # 在 CI/CD 中集成
   # .github/workflows/performance.yml

   - name: Performance Test
     run: |
       cd build/test
       ./test_static_file_server &
       sleep 2
       cd ../..
       ./scripts/S5-TestRangeEtagStress.sh > perf_results.txt

       # 检查性能指标
       THROUGHPUT=$(grep "Requests per second" perf_results.txt | head -1 | awk '{print $4}')
       if (( $(echo "$THROUGHPUT < 10000" | bc -l) )); then
         echo "Performance regression detected!"
         exit 1
       fi
   ```

3. **持续性能监控**：
   ```bash
   # 定期运行并记录结果
   while true; do
     DATE=$(date +%Y%m%d_%H%M%S)
     ./scripts/S5-TestRangeEtagStress.sh > "stress_${DATE}.txt" 2>&1

     # 提取关键指标
     grep "Requests per second" "stress_${DATE}.txt" >> performance_history.csv

     # 等待 1 小时
     sleep 3600
   done
   ```

## 脚本实现细节

### 关键变量

```bash
SERVER="http://localhost:8080"      # 服务器地址
TEST_FILE="/static/test.txt"        # 测试文件路径
TEST_URL="${SERVER}${TEST_FILE}"    # 完整测试 URL
```

### ab 工具检查

```bash
if ! command -v ab &> /dev/null; then
    echo "错误: 未找到 Apache Bench (ab)"
    echo "请安装: brew install httpd (macOS) 或 apt-get install apache2-utils (Linux)"
    exit 1
fi
```

### ETag 提取

```bash
ETAG=$(curl -s -I "$TEST_URL" | grep -i "ETag:" | cut -d' ' -f2 | tr -d '\r')
echo "ETag: $ETAG"
```

### 测试执行

```bash
echo "=== 压力测试 N: 测试描述 ==="
echo "命令: ab -n 10000 -c 100 ..."
echo ""
ab -n 10000 -c 100 [选项] "$TEST_URL"
echo ""
echo "✓ 压力测试 N 完成"
echo ""
```

## 扩展功能

### 自定义测试参数

可以修改脚本中的测试参数：

```bash
# 增加请求数和并发数
ab -n 50000 -c 200 "$TEST_URL"

# 添加超时设置
ab -n 10000 -c 100 -s 30 "$TEST_URL"  # 30 秒超时

# 使用 Keep-Alive
ab -n 10000 -c 100 -k "$TEST_URL"
```

### 生成性能报告

可以提取关键指标生成报告：

```bash
# 提取吞吐量
grep "Requests per second" stress_test.txt | awk '{print $4}'

# 提取 P99 延迟
grep "99%" stress_test.txt | awk '{print $2}'

# 生成 CSV 报告
echo "Test,Throughput,P99_Latency" > report.csv
echo "Normal,$THROUGHPUT1,$P99_1" >> report.csv
echo "Range,$THROUGHPUT2,$P99_2" >> report.csv
```

### 集成监控工具

可以结合系统监控：

```bash
# 同时监控系统资源
./scripts/S5-TestRangeEtagStress.sh &
STRESS_PID=$!

# 监控 CPU 和内存
while kill -0 $STRESS_PID 2>/dev/null; do
    ps aux | grep test_static_file_server | grep -v grep
    sleep 1
done
```

## 相关脚本

- **S1-Run.sh**：运行测试和示例的通用脚本
- **S2-Check.sh**：验证测试结果和压测指标
- **S3-BenchmarkStaticFiles.sh**：静态文件服务压测
- **S4-TestRangeEtagManual.sh**：HTTP Range 和 ETag 手动测试

## 技术规格

| 项目 | 说明 |
|------|------|
| Shell 版本 | Bash 3.2+ |
| 依赖工具 | ab (Apache Bench), curl |
| 支持平台 | Linux, macOS |
| 脚本位置 | `scripts/S5-TestRangeEtagStress.sh` |
| 测试场景数 | 5 个 |
| 默认请求数 | 10,000（多范围测试 1,000） |
| 默认并发数 | 100（多范围测试 50） |

## 参考资料

### 性能测试工具

- **Apache Bench (ab)**：简单易用的 HTTP 压测工具
- **wrk**：更强大的现代压测工具
- **siege**：支持多 URL 的压测工具
- **JMeter**：功能全面的性能测试工具

### 性能优化资源

- **HTTP/1.1 性能优化**：Keep-Alive、Pipeline、压缩
- **HTTP/2 优化**：多路复用、服务器推送
- **系统调优**：文件描述符、TCP 参数、内核参数

---

**文档版本**：v1.0
**创建日期**：2026-01-29
**维护团队**：galay-http 开发团队
