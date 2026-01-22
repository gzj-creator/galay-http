#!/bin/bash

# HTTP Range 和 ETag 手动测试脚本
# 使用 curl 测试服务器的 Range 和 ETag 功能

SERVER="http://localhost:8080"
TEST_FILE="/static/test.txt"
TEST_URL="${SERVER}${TEST_FILE}"

echo "========================================"
echo "HTTP Range and ETag Manual Tests"
echo "========================================"
echo ""
echo "请先启动测试服务器："
echo "  cd build/test && ./test_static_file_server"
echo ""
echo "按 Enter 继续测试..."
read

echo ""
echo "=== Test 1: 获取完整文件并查看 ETag ==="
echo "命令: curl -i $TEST_URL"
echo ""
RESPONSE=$(curl -i -s "$TEST_URL")
echo "$RESPONSE"
ETAG=$(echo "$RESPONSE" | grep -i "ETag:" | cut -d' ' -f2 | tr -d '\r')
echo ""
echo "提取的 ETag: $ETAG"
echo "✓ Test 1 完成"
echo ""

echo "=== Test 2: 使用 If-None-Match 测试 304 响应 ==="
echo "命令: curl -i -H \"If-None-Match: $ETAG\" $TEST_URL"
echo ""
curl -i -H "If-None-Match: $ETAG" "$TEST_URL"
echo ""
echo "✓ Test 2 完成（应该看到 304 Not Modified）"
echo ""

echo "=== Test 3: 单范围请求（前 100 字节）==="
echo "命令: curl -i -H \"Range: bytes=0-99\" $TEST_URL"
echo ""
curl -i -H "Range: bytes=0-99" "$TEST_URL"
echo ""
echo "✓ Test 3 完成（应该看到 206 Partial Content 和 Content-Range: bytes 0-99/...）"
echo ""

echo "=== Test 4: 后缀范围请求（最后 100 字节）==="
echo "命令: curl -i -H \"Range: bytes=-100\" $TEST_URL"
echo ""
curl -i -H "Range: bytes=-100" "$TEST_URL"
echo ""
echo "✓ Test 4 完成（应该看到 206 Partial Content）"
echo ""

echo "=== Test 5: 前缀范围请求（从 500 字节到末尾）==="
echo "命令: curl -i -H \"Range: bytes=500-\" $TEST_URL"
echo ""
curl -i -H "Range: bytes=500-" "$TEST_URL"
echo ""
echo "✓ Test 5 完成（应该看到 206 Partial Content）"
echo ""

echo "=== Test 6: 多范围请求 ==="
echo "命令: curl -i -H \"Range: bytes=0-49,100-149,200-249\" $TEST_URL"
echo ""
curl -i -H "Range: bytes=0-49,100-149,200-249" "$TEST_URL"
echo ""
echo "✓ Test 6 完成（应该看到 206 Partial Content 和 multipart/byteranges）"
echo ""

echo "=== Test 7: If-Range 条件请求（ETag 匹配）==="
echo "命令: curl -i -H \"Range: bytes=0-99\" -H \"If-Range: $ETAG\" $TEST_URL"
echo ""
curl -i -H "Range: bytes=0-99" -H "If-Range: $ETAG" "$TEST_URL"
echo ""
echo "✓ Test 7 完成（应该看到 206 Partial Content）"
echo ""

echo "=== Test 8: If-Range 条件请求（ETag 不匹配）==="
echo "命令: curl -i -H \"Range: bytes=0-99\" -H \"If-Range: \\\"wrong-etag\\\"\" $TEST_URL"
echo ""
curl -i -H "Range: bytes=0-99" -H "If-Range: \"wrong-etag\"" "$TEST_URL"
echo ""
echo "✓ Test 8 完成（应该看到 200 OK，返回完整文件）"
echo ""

echo "=== Test 9: 无效范围请求（416 响应）==="
echo "命令: curl -i -H \"Range: bytes=99999-999999\" $TEST_URL"
echo ""
curl -i -H "Range: bytes=99999-999999" "$TEST_URL"
echo ""
echo "✓ Test 9 完成（应该看到 416 Range Not Satisfiable）"
echo ""

echo "========================================"
echo "所有手动测试完成！"
echo "========================================"
