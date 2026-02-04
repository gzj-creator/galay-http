#!/bin/bash

echo "=========================================="
echo "清理旧的构建文件..."
echo "=========================================="
cd build
rm -rf *

echo ""
echo "=========================================="
echo "配置 CMake (Debug 模式)..."
echo "=========================================="
cmake .. -DCMAKE_BUILD_TYPE=Debug

echo ""
echo "=========================================="
echo "检查 ENABLE_DEBUG 宏是否被定义..."
echo "=========================================="
echo "查找编译命令中的 -DENABLE_DEBUG 标志："
make VERBOSE=1 2>&1 | grep -o "\-DENABLE_DEBUG" | head -5

echo ""
echo "=========================================="
echo "开始编译..."
echo "=========================================="
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

echo ""
echo "=========================================="
echo "编译完成！"
echo "=========================================="
echo "现在运行您的程序，Debug 日志应该会输出。"
echo ""
echo "如果还是没有日志输出，请检查："
echo "1. 日志级别设置（spdlog 的 set_level）"
echo "2. 是否真的调用了 HTTP_LOG_DEBUG 宏"
echo "3. 运行 'make VERBOSE=1' 查看完整编译命令"
