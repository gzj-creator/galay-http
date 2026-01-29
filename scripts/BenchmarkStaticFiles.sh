#!/bin/bash

# 静态文件服务压测脚本
# 使用 wrk 或 ab (Apache Bench) 进行压测

set -e

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 配置
SERVER_HOST="localhost"
SERVER_PORT="8080"
BASE_URL="http://${SERVER_HOST}:${SERVER_PORT}"

# 压测参数
DURATION="30s"
THREADS=4
CONNECTIONS=100

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}Static File Server Benchmark${NC}"
echo -e "${BLUE}========================================${NC}"
echo -e "Server: ${BASE_URL}"
echo -e "Duration: ${DURATION}"
echo -e "Threads: ${THREADS}"
echo -e "Connections: ${CONNECTIONS}"
echo -e "${BLUE}========================================${NC}\n"

# 检查服务器是否运行
echo -e "${YELLOW}Checking if server is running...${NC}"
if ! curl -s "${BASE_URL}/api/status" > /dev/null 2>&1; then
    echo -e "${RED}Error: Server is not running at ${BASE_URL}${NC}"
    echo -e "${YELLOW}Please start the server first:${NC}"
    echo -e "  cd build && ./test_static_file_server"
    exit 1
fi
echo -e "${GREEN}✓ Server is running${NC}\n"

# 检查压测工具
TOOL=""
if command -v wrk &> /dev/null; then
    TOOL="wrk"
    echo -e "${GREEN}Using wrk for benchmarking${NC}\n"
elif command -v ab &> /dev/null; then
    TOOL="ab"
    echo -e "${GREEN}Using Apache Bench (ab) for benchmarking${NC}\n"
else
    echo -e "${RED}Error: Neither wrk nor ab found${NC}"
    echo -e "${YELLOW}Please install one of them:${NC}"
    echo -e "  macOS: brew install wrk"
    echo -e "  Linux: apt-get install apache2-utils (for ab)"
    exit 1
fi

# 压测函数
benchmark() {
    local name=$1
    local url=$2
    local file_size=$3

    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}Testing: ${name}${NC}"
    echo -e "${BLUE}URL: ${url}${NC}"
    if [ -n "$file_size" ]; then
        echo -e "${BLUE}File Size: ${file_size}${NC}"
    fi
    echo -e "${BLUE}========================================${NC}"

    if [ "$TOOL" = "wrk" ]; then
        wrk -t${THREADS} -c${CONNECTIONS} -d${DURATION} --latency "${url}"
    else
        # ab 使用请求数而不是持续时间
        local requests=$((CONNECTIONS * 1000))
        ab -n ${requests} -c ${CONNECTIONS} -g /dev/null "${url}"
    fi

    echo -e "\n"
    sleep 2
}

# 1. 测试主页
benchmark "Homepage" "${BASE_URL}/" ""

# 2. 测试 API
benchmark "API Status" "${BASE_URL}/api/status" ""

# 3. 测试小文件 (HTML)
benchmark "HTML File (Dynamic)" "${BASE_URL}/static/index.html" "~1KB"

# 4. 测试小文件 (Static)
benchmark "HTML File (Static)" "${BASE_URL}/files/index.html" "~1KB"

# 5. 测试 CSS 文件
benchmark "CSS File (Dynamic)" "${BASE_URL}/static/css/style.css" "~200B"

# 6. 测试 JS 文件
benchmark "JS File (Static)" "${BASE_URL}/files/js/app.js" "~100B"

# 7. 测试 JSON 文件
benchmark "JSON File (Dynamic)" "${BASE_URL}/static/docs/data.json" "~150B"

# 8. 测试小二进制文件 (10KB)
benchmark "Small Binary File (10KB)" "${BASE_URL}/static/small.bin" "10KB"

# 9. 测试中等文件 (1MB)
benchmark "Medium Binary File (1MB)" "${BASE_URL}/static/medium.bin" "1MB"

# 10. 测试大文件 (10MB)
benchmark "Large Binary File (10MB)" "${BASE_URL}/static/large.bin" "10MB"

# 11. 对比动态 vs 静态模式 (小文件)
echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}Comparison: Dynamic vs Static Mode${NC}"
echo -e "${BLUE}========================================${NC}\n"

echo -e "${YELLOW}Dynamic Mode (mount):${NC}"
benchmark "Dynamic - Small File" "${BASE_URL}/static/small.bin" "10KB"

echo -e "${YELLOW}Static Mode (mountHardly):${NC}"
benchmark "Static - Small File" "${BASE_URL}/files/small.bin" "10KB"

# 完成
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Benchmark completed!${NC}"
echo -e "${GREEN}========================================${NC}"

# 获取最终状态
echo -e "\n${YELLOW}Final server status:${NC}"
curl -s "${BASE_URL}/api/status" | python3 -m json.tool 2>/dev/null || curl -s "${BASE_URL}/api/status"
echo -e "\n"
