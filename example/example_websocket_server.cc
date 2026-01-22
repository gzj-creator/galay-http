/**
 * @file example_websocket_server.cc
 * @brief WebSocket 服务器完整示例
 * @details 展示如何使用 WsUpgrade、WsHeartbeat 和控制帧回调
 */

#include <iostream>
#include <memory>
#include "galay-http/kernel/http/HttpServer.h"
#include "galay-http/kernel/websocket/WsUpgrade.h"
#include "galay-http/kernel/websocket/WsHeartbeat.h"
#include "galay-http/kernel/websocket/WsConn.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-kernel/common/Log.h"

#ifdef USE_KQUEUE
#include "galay-kernel/kernel/KqueueScheduler.h"
using IOSchedulerType = galay::kernel::KqueueScheduler;
#endif

#ifdef USE_EPOLL
#include "galay-kernel/kernel/EpollScheduler.h"
using IOSchedulerType = galay::kernel::EpollScheduler;
#endif

#ifdef USE_IOURING
#include "galay-kernel/kernel/IOUringScheduler.h"
using IOSchedulerType = galay::kernel::IOUringScheduler;
#endif

using namespace galay::http;
using namespace galay::websocket;
using namespace galay::kernel;

// ==================== WebSocket 处理器 ====================

/**
 * @brief 处理 WebSocket 连接
 * @param ws_conn WebSocket 连接（通过引用传递，因为 WsConn 禁用了移动）
 */
Coroutine handleWebSocketConnection(WsConn& ws_conn) {
    LogInfo("WebSocket connection established");

    // 配置心跳
    WsHeartbeatConfig heartbeat_config;
    heartbeat_config.enabled = true;
    heartbeat_config.ping_interval = std::chrono::seconds(30);
    heartbeat_config.pong_timeout = std::chrono::seconds(10);
    heartbeat_config.auto_close_on_timeout = true;

    // 创建心跳管理器
    WsHeartbeat heartbeat(ws_conn, heartbeat_config);

    // 获取 Reader 并设置控制帧回调
    auto reader = ws_conn.getReader();
    reader.setControlFrameCallback([&heartbeat](WsOpcode opcode, const std::string& payload) {
        if (opcode == WsOpcode::Pong) {
            LogDebug("Received Pong frame");
            heartbeat.onPongReceived();
        } else if (opcode == WsOpcode::Ping) {
            LogDebug("Received Ping frame");
            // 服务器收到 Ping 时应该自动回复 Pong（这里简化处理）
        } else if (opcode == WsOpcode::Close) {
            LogInfo("Received Close frame");
        }
    });

    // 启动心跳协程（在后台运行）
    // 注意：这里需要使用 spawn 来启动后台协程
    // heartbeat.start(); // 暂时注释，因为需要调度器支持

    // 获取 Writer
    auto writer = ws_conn.getWriter();

    // 发送欢迎消息
    WsFrame welcome_frame;
    welcome_frame.header.fin = true;
    welcome_frame.header.opcode = WsOpcode::Text;
    welcome_frame.payload = "Welcome to WebSocket server!";
    welcome_frame.header.payload_length = welcome_frame.payload.size();

    auto send_result = co_await writer.sendFrame(welcome_frame);
    if (!send_result) {
        LogError("Failed to send welcome message: {}", send_result.error().message());
        co_return;
    }

    // 消息循环
    while (true) {
        std::string message;
        WsOpcode opcode;

        // 读取消息
        auto result = co_await reader.getMessage(message, opcode);

        if (!result.has_value()) {
            WsError error = result.error();
            if (error.code() == kWsConnectionClosed) {
                LogInfo("WebSocket connection closed by peer");
                break;
            }
            LogError("Failed to read message: {}", error.message());
            break;
        }

        if (!result.value()) {
            // 消息不完整，继续读取
            continue;
        }

        // 处理消息
        LogInfo("Received message (opcode={}): {}",
                static_cast<int>(opcode),
                message.substr(0, std::min(message.size(), size_t(100))));

        // 回显消息
        WsFrame echo_frame;
        echo_frame.header.fin = true;
        echo_frame.header.opcode = opcode;
        echo_frame.payload = "Echo: " + message;
        echo_frame.header.payload_length = echo_frame.payload.size();

        auto echo_result = co_await writer.sendFrame(echo_frame);
        if (!echo_result) {
            LogError("Failed to send echo message: {}", echo_result.error().message());
            break;
        }
    }

    // 关闭连接
    LogInfo("Closing WebSocket connection");
    co_await ws_conn.close();
    co_return;
}

/**
 * @brief HTTP 请求处理器（处理 WebSocket 升级）
 * @param conn HTTP 连接
 */
Coroutine handleHttpRequest(HttpConn conn) {
    // 读取 HTTP 请求
    auto reader = conn.getReader();
    HttpRequest request;

    auto read_result = co_await reader.getRequest(request);
    if (!read_result) {
        LogError("Failed to read HTTP request: {}", read_result.error().message());
        co_await conn.close();
        co_return;
    }

    LogInfo("Received HTTP request: {} {}",
            httpMethodToString(request.header().method()),
            request.header().uri());

    // 检查是否是 WebSocket 升级请求
    if (request.header().uri() == "/ws") {
        // 处理 WebSocket 升级
        auto upgrade_result = WsUpgrade::handleUpgrade(request);

        if (!upgrade_result.success) {
            LogError("WebSocket upgrade failed: {}", upgrade_result.error_message);

            // 发送错误响应
            auto writer = conn.getWriter();
            co_await writer.sendResponse(upgrade_result.response);
            co_await conn.close();
            co_return;
        }

        LogInfo("WebSocket upgrade successful");

        // 发送 101 Switching Protocols 响应
        auto writer = conn.getWriter();
        auto send_result = co_await writer.sendResponse(upgrade_result.response);

        if (!send_result) {
            LogError("Failed to send upgrade response: {}", send_result.error().message());
            co_await conn.close();
            co_return;
        }

        // 升级到 WebSocket 连接
        WsReaderSetting reader_setting;
        reader_setting.max_frame_size = 1024 * 1024;  // 1MB
        reader_setting.max_message_size = 10 * 1024 * 1024;  // 10MB

        WsWriterSetting writer_setting;

        // 创建 WebSocket 连接（转移 socket 和 ring_buffer）
        WsConn ws_conn(
            std::move(conn.socket()),
            std::move(conn.ringBuffer()),
            reader_setting,
            writer_setting,
            true  // is_server
        );

        // 处理 WebSocket 连接（通过引用传递）
        co_await handleWebSocketConnection(ws_conn).wait();
        co_return;
    }

    // 普通 HTTP 请求
    HttpResponse response;
    response.header().version() = HttpVersion::HttpVersion_1_1;
    response.header().code() = HttpStatusCode::OK_200;
    response.header().headerPairs().addHeaderPair("Content-Type", "text/html; charset=utf-8");

    std::string body = R"(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>WebSocket Example</title>
</head>
<body>
    <h1>WebSocket Server Example</h1>
    <p>Connect to WebSocket endpoint: <code>ws://localhost:8080/ws</code></p>

    <h2>Test with JavaScript:</h2>
    <pre>
const ws = new WebSocket('ws://localhost:8080/ws');

ws.onopen = () => {
    console.log('Connected');
    ws.send('Hello Server!');
};

ws.onmessage = (event) => {
    console.log('Received:', event.data);
};

ws.onerror = (error) => {
    console.error('Error:', error);
};

ws.onclose = () => {
    console.log('Disconnected');
};
    </pre>
</body>
</html>)";

    response.header().headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));
    response.setBodyStr(std::move(body));

    auto writer = conn.getWriter();
    co_await writer.sendResponse(response);
    co_await conn.close();
    co_return;
}

// ==================== 主函数 ====================

int main() {
    LogInfo("========================================");
    LogInfo("WebSocket Server Example");
    LogInfo("========================================\n");

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
    // 配置服务器
    HttpServerConfig config;
    config.host = "0.0.0.0";
    config.port = 8080;
    config.backlog = 128;
    config.io_scheduler_count = 4;
    config.compute_scheduler_count = 2;

    // 创建 HTTP 服务器
    HttpServer server(config);

    // 启动服务器
    LogInfo("Starting WebSocket server on {}:{}", config.host, config.port);
    LogInfo("WebSocket endpoint: ws://localhost:8080/ws");
    LogInfo("HTTP endpoint: http://localhost:8080/");
    LogInfo("Press Ctrl+C to stop\n");

    // 启动服务器并传入处理器
    server.start(handleHttpRequest);

    // 保持服务器运行
    LogInfo("Server is running. Press Ctrl+C to stop.");
    while (server.isRunning()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    LogInfo("Server stopped");
    return 0;
#else
    LogError("No scheduler defined. Please compile with -DUSE_KQUEUE, -DUSE_EPOLL, or -DUSE_IOURING");
    return 1;
#endif
}
