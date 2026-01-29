#!/bin/bash
# check.sh - 验证测试结果和压测指标的通用脚本
# 用法: ./scripts/check.sh [选项]

set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 打印帮助信息
print_help() {
    echo "用法: $0 [选项]"
    echo ""
    echo "选项:"
    echo "  --build        - 检查构建状态"
    echo "  --tests        - 检查测试文件"
    echo "  --benchmarks   - 检查压测文件"
    echo "  --docs         - 检查文档完整性"
    echo "  --naming       - 检查命名规范"
    echo "  --all          - 执行所有检查（默认）"
    echo "  -h, --help     - 显示帮助信息"
}

# 检查构建状态
check_build() {
    echo -e "${BLUE}检查构建状态...${NC}"

    if [ ! -d "$BUILD_DIR" ]; then
        echo -e "${RED}✗ 构建目录不存在${NC}"
        return 1
    fi

    local required_dirs=("test" "example" "benchmark")
    local all_exist=true

    for dir in "${required_dirs[@]}"; do
        if [ ! -d "${BUILD_DIR}/${dir}" ]; then
            echo -e "${RED}✗ 缺少目录: ${dir}${NC}"
            all_exist=false
        fi
    done

    if $all_exist; then
        echo -e "${GREEN}✓ 构建目录结构完整${NC}"
        return 0
    else
        return 1
    fi
}

# 检查测试文件
check_tests() {
    echo -e "${BLUE}检查测试文件...${NC}"

    local test_dir="${PROJECT_ROOT}/test"
    local test_count=$(find "$test_dir" -name "T*.cc" | wc -l)

    echo -e "  测试文件数量: ${test_count}"

    # 检查命名规范
    local invalid_names=$(find "$test_dir" -name "*.cc" ! -name "T*-*.cc" -type f | wc -l)

    if [ "$invalid_names" -gt 0 ]; then
        echo -e "${YELLOW}⚠ 发现 ${invalid_names} 个不符合命名规范的测试文件${NC}"
        find "$test_dir" -name "*.cc" ! -name "T*-*.cc" -type f
        return 1
    fi

    echo -e "${GREEN}✓ 所有测试文件命名符合规范 (T1-T${test_count})${NC}"
    return 0
}

# 检查压测文件
check_benchmarks() {
    echo -e "${BLUE}检查压测文件...${NC}"

    local bench_dir="${PROJECT_ROOT}/benchmark"
    local bench_count=$(find "$bench_dir" -name "B*.cc" | wc -l)

    echo -e "  压测文件数量: ${bench_count}"

    # 检查命名规范
    local invalid_names=$(find "$bench_dir" -name "*.cc" ! -name "B*-*.cc" -type f | wc -l)

    if [ "$invalid_names" -gt 0 ]; then
        echo -e "${YELLOW}⚠ 发现 ${invalid_names} 个不符合命名规范的压测文件${NC}"
        find "$bench_dir" -name "*.cc" ! -name "B*-*.cc" -type f
        return 1
    fi

    echo -e "${GREEN}✓ 所有压测文件命名符合规范 (B1-B${bench_count})${NC}"
    return 0
}

# 检查文档完整性
check_docs() {
    echo -e "${BLUE}检查文档完整性...${NC}"

    local docs_dir="${PROJECT_ROOT}/docs"
    local doc_count=$(find "$docs_dir" -name "*.md" | wc -l)

    echo -e "  文档文件数量: ${doc_count}"

    # 检查是否有 README
    if [ ! -f "${PROJECT_ROOT}/README.md" ]; then
        echo -e "${RED}✗ 缺少 README.md${NC}"
        return 1
    fi

    echo -e "${GREEN}✓ 文档目录存在，包含 ${doc_count} 个文档${NC}"
    return 0
}

# 检查命名规范
check_naming() {
    echo -e "${BLUE}检查命名规范...${NC}"

    local issues=0

    # 检查测试文件
    echo -e "  检查测试文件命名..."
    local test_files=$(find "${PROJECT_ROOT}/test" -name "*.cc" ! -name "T*-*.cc" -type f)
    if [ -n "$test_files" ]; then
        echo -e "${RED}✗ 测试文件命名不规范:${NC}"
        echo "$test_files"
        ((issues++))
    else
        echo -e "${GREEN}  ✓ 测试文件命名规范${NC}"
    fi

    # 检查压测文件
    echo -e "  检查压测文件命名..."
    local bench_files=$(find "${PROJECT_ROOT}/benchmark" -name "*.cc" ! -name "B*-*.cc" -type f)
    if [ -n "$bench_files" ]; then
        echo -e "${RED}✗ 压测文件命名不规范:${NC}"
        echo "$bench_files"
        ((issues++))
    else
        echo -e "${GREEN}  ✓ 压测文件命名规范${NC}"
    fi

    # 检查示例文件
    echo -e "  检查示例文件命名..."
    local example_files=$(find "${PROJECT_ROOT}/example" -name "*.cc" ! -name "E*-*.cc" -type f)
    if [ -n "$example_files" ]; then
        echo -e "${RED}✗ 示例文件命名不规范:${NC}"
        echo "$example_files"
        ((issues++))
    else
        echo -e "${GREEN}  ✓ 示例文件命名规范${NC}"
    fi

    if [ $issues -eq 0 ]; then
        echo -e "${GREEN}✓ 所有文件命名符合规范${NC}"
        return 0
    else
        return 1
    fi
}

# 执行所有检查
check_all() {
    local failed=0

    check_build || ((failed++))
    echo ""

    check_tests || ((failed++))
    echo ""

    check_benchmarks || ((failed++))
    echo ""

    check_docs || ((failed++))
    echo ""

    check_naming || ((failed++))
    echo ""

    echo "========================================"
    if [ $failed -eq 0 ]; then
        echo -e "${GREEN}✓ 所有检查通过${NC}"
        return 0
    else
        echo -e "${RED}✗ ${failed} 项检查失败${NC}"
        return 1
    fi
}

# 主逻辑
main() {
    if [ $# -eq 0 ]; then
        check_all
        exit $?
    fi

    case "$1" in
        --build)
            check_build
            ;;
        --tests)
            check_tests
            ;;
        --benchmarks)
            check_benchmarks
            ;;
        --docs)
            check_docs
            ;;
        --naming)
            check_naming
            ;;
        --all)
            check_all
            ;;
        -h|--help)
            print_help
            ;;
        *)
            echo -e "${RED}错误: 未知选项 '$1'${NC}"
            print_help
            exit 1
            ;;
    esac
}

main "$@"
