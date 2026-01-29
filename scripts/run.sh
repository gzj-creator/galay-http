#!/bin/bash
# run.sh - 运行测试和示例的通用脚本
# 用法: ./scripts/run.sh [test|example|benchmark] [文件名]

set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 打印帮助信息
print_help() {
    echo "用法: $0 [类型] [文件名]"
    echo ""
    echo "类型:"
    echo "  test       - 运行测试文件"
    echo "  example    - 运行示例文件"
    echo "  benchmark  - 运行压测文件"
    echo "  all-tests  - 运行所有测试"
    echo ""
    echo "示例:"
    echo "  $0 test T1-HttpParser"
    echo "  $0 example E1-EchoServer"
    echo "  $0 benchmark B1-Chunked"
    echo "  $0 all-tests"
}

# 检查构建目录
check_build() {
    if [ ! -d "$BUILD_DIR" ]; then
        echo -e "${RED}错误: 构建目录不存在，请先运行 cmake 构建项目${NC}"
        exit 1
    fi
}

# 运行单个可执行文件
run_executable() {
    local type=$1
    local name=$2
    local exec_path="${BUILD_DIR}/${type}/${name}"

    if [ ! -f "$exec_path" ]; then
        echo -e "${RED}错误: 可执行文件不存在: ${exec_path}${NC}"
        echo -e "${YELLOW}提示: 请先编译项目${NC}"
        exit 1
    fi

    echo -e "${GREEN}运行: ${exec_path}${NC}"
    echo "========================================"
    "$exec_path" "$@"
}

# 运行所有测试
run_all_tests() {
    echo -e "${GREEN}运行所有测试...${NC}"
    local test_dir="${BUILD_DIR}/test"

    if [ ! -d "$test_dir" ]; then
        echo -e "${RED}错误: 测试目录不存在${NC}"
        exit 1
    fi

    local passed=0
    local failed=0

    for test_file in "$test_dir"/T*; do
        if [ -x "$test_file" ]; then
            local test_name=$(basename "$test_file")
            echo ""
            echo -e "${YELLOW}运行测试: ${test_name}${NC}"
            echo "========================================"

            if "$test_file"; then
                echo -e "${GREEN}✓ ${test_name} 通过${NC}"
                ((passed++))
            else
                echo -e "${RED}✗ ${test_name} 失败${NC}"
                ((failed++))
            fi
        fi
    done

    echo ""
    echo "========================================"
    echo -e "${GREEN}通过: ${passed}${NC}"
    echo -e "${RED}失败: ${failed}${NC}"

    if [ $failed -gt 0 ]; then
        exit 1
    fi
}

# 主逻辑
main() {
    check_build

    if [ $# -eq 0 ]; then
        print_help
        exit 1
    fi

    local type=$1
    shift

    case "$type" in
        test)
            if [ $# -eq 0 ]; then
                echo -e "${RED}错误: 请指定测试文件名${NC}"
                exit 1
            fi
            run_executable "test" "$@"
            ;;
        example)
            if [ $# -eq 0 ]; then
                echo -e "${RED}错误: 请指定示例文件名${NC}"
                exit 1
            fi
            run_executable "example" "$@"
            ;;
        benchmark)
            if [ $# -eq 0 ]; then
                echo -e "${RED}错误: 请指定压测文件名${NC}"
                exit 1
            fi
            run_executable "benchmark" "$@"
            ;;
        all-tests)
            run_all_tests
            ;;
        -h|--help)
            print_help
            ;;
        *)
            echo -e "${RED}错误: 未知类型 '$type'${NC}"
            print_help
            exit 1
            ;;
    esac
}

main "$@"
