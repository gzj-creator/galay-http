#!/bin/bash

# HTTP Range 和 ETag 压力测试脚本
# 使用 Apache Bench (ab) 或 wrk 进行压力测试

SERVER="http://localhost:8080"
TEST_FILE="/static/test.txt"
TEST_URL="${SERVER}${TEST_FILE}"

echo "========================================"
echo "HTTP Range and ETag Stress Tests"
echo "========================================"
echo ""
echo "请先启动测试服务器："
echo "  cd build/test && ./test_static_file_server"
echo ""
echo "按 Enter 继续压力测试..."
read

# 检查是否安装了 ab
if ! command -v ab &> /dev/null; then
    echo "错误: 未找到 Apache Bench (ab)"
    echo "请安装: brew install httpd (macOS) 或 apt-get install apache2-utils (Linux)"
    exit 1
fi

echo ""
echo "=== 压力测试 1: 普通请求（无 Range）==="
echo "命令: ab -n 10000 -c 100 $TEST_URL"
echo ""
ab -n 10000 -c 100 "$TEST_URL"
echo ""
echo "✓ 压力测试 1 完成"
echo ""

echo "=== 压力测试 2: Range 请求（前 100 字节）==="
echo "命令: ab -n 10000 -c 100 -H \"Range: bytes=0-99\" $TEST_URL"
echo ""
ab -n 10000 -c 100 -H "Range: bytes=0-99" "$TEST_URL"
echo ""
echo "✓ 压力测试 2 完成"
echo ""

echo "=== 压力测试 3: 带 ETag 的条件请求 ==="
echo "首先获取 ETag..."
ETAG=$(curl -s -I "$TEST_URL" | grep -i "ETag:" | cut -d' ' -f2 | tr -d '\r')
echo "ETag: $ETAG"
echo ""
echo "命令: ab -n 10000 -c 100 -H \"If-None-Match: $ETAG\" $TEST_URL"
echo ""
ab -n 10000 -c 100 -H "If-None-Match: $ETAG" "$TEST_URL"
echo ""
echo "✓ 压力测试 3 完成（大部分应该是 304 响应）"
echo ""

echo "=== 压力测试 4: 多范围请求 ==="
echo "命令: ab -n 1000 -c 50 -H \"Range: bytes=0-49,100-149\" $TEST_URL"
echo ""
ab -n 1000 -c 50 -H "Range: bytes=0-49,100-149" "$TEST_URL"
echo ""
echo "✓ 压力测试 4 完成"
echo ""

echo "=== 压力测试 5: If-Range 条件请求 ==="
echo "命令: ab -n 10000 -c 100 -H \"Range: bytes=0-99\" -H \"If-Range: $ETAG\" $TEST_URL"
echo ""
ab -n 10000 -c 100 -H "Range: bytes=0-99" -H "If-Range: $ETAG" "$TEST_URL"
echo ""
echo "✓ 压力测试 5 完成"
echo ""

echo "========================================"
echo "所有压力测试完成！"
echo "========================================"
echo ""
echo "性能总结："
echo "- 测试 1: 普通请求基准性能"
echo "- 测试 2: Range 请求性能（应该略低于普通请求）"
echo "- 测试 3: ETag 304 响应性能（应该非常快，无文件传输）"
echo "- 测试 4: 多范围请求性能（最慢，需要多次文件读取）"
echo "- 测试 5: If-Range 条件请求性能"
echo ""
