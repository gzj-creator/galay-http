#!/bin/bash

# WebSocket 测试脚本
# 用于测试 WebSocket 服务器和客户端的通信

echo "=========================================="
echo "WebSocket 测试脚本"
echo "=========================================="

# 检查构建目录
if [ ! -d "build" ]; then
    echo "错误: build 目录不存在，请先编译项目"
    echo "运行: mkdir build && cd build && cmake .. && make"
    exit 1
fi

# 检查可执行文件
if [ ! -f "build/example/example_websocket_server" ]; then
    echo "错误: example_websocket_server 不存在，请先编译"
    exit 1
fi

if [ ! -f "build/example/example_websocket_client" ]; then
    echo "错误: example_websocket_client 不存在，请先编译"
    exit 1
fi

# 启动服务器
echo ""
echo "1. 启动 WebSocket 服务器..."
./build/example/example_websocket_server &
SERVER_PID=$!
echo "   服务器 PID: $SERVER_PID"

# 等待服务器启动
echo "   等待服务器启动..."
sleep 2

# 检查服务器是否运行
if ! ps -p $SERVER_PID > /dev/null; then
    echo "   错误: 服务器启动失败"
    exit 1
fi

echo "   服务器启动成功"

# 启动客户端
echo ""
echo "2. 启动 WebSocket 客户端..."
./build/example/example_websocket_client

# 等待客户端完成
echo ""
echo "3. 客户端执行完成"

# 停止服务器
echo ""
echo "4. 停止服务器..."
kill $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null

echo ""
echo "=========================================="
echo "测试完成"
echo "=========================================="
