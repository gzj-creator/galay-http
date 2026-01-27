/**
 * @file example_websocket_server.cc
 * @brief WebSocket 服务器完整示例
 * @details 展示如何使用 WsUpgrade 和手动处理控制帧（Ping/Pong/Close）
 */

#include <chrono>
#include "galay-http/kernel/http/HttpServer.h"
#include "galay-http/kernel/websocket/WsUpgrade.h"
#include "galay-http/kernel/websocket/WsConn.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/kernel/http/HttpLog.h"

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
using namespace std::chrono_literals;

// ==================== WebSocket 处理器 ====================

/**
 * @brief 处理 WebSocket 连接
 * @param ws_conn WebSocket 连接（通过引用传递）
 */
Coroutine handleWebSocketConnection(WsConn& ws_conn) {
    HTTP_LOG_INFO("WebSocket connection established");

    // 获取 Reader 和 Writer（必须在协程开始时获取，保证 ws_conn 生命周期）
    auto& reader = ws_conn.getReader();
    auto& writer = ws_conn.getWriter();

    // 发送欢迎消息
    HTTP_LOG_INFO("Sending welcome message");
    auto send_result = co_await writer.sendText("Welcome to WebSocket server!");
    if (!send_result) {
        HTTP_LOG_ERROR("Failed to send welcome message: {}", send_result.error().message());
        co_return;
    }
    HTTP_LOG_INFO("Welcome message sent");

    // 消息循环
    HTTP_LOG_INFO("Entering message loop");
    while (true) {
        std::string message;
        WsOpcode opcode;

        // 读取消息（包括数据帧和控制帧）
        auto result = co_await reader.getMessage(message, opcode).timeout(1000ms);

        if (!result.has_value()) {
            WsError error = result.error();
            if (error.code() == kWsConnectionClosed) {
                HTTP_LOG_INFO("WebSocket connection closed by peer");
                break;
            }
            HTTP_LOG_ERROR("Failed to read message: {}", error.message());
            break;
        }

        if (!result.value()) {
            // 消息不完整，继续读取
            continue;
        }

        // 根据 opcode 判断消息类型并处理
        if (opcode == WsOpcode::Ping) {
            // 收到 Ping，发送 Pong 响应
            HTTP_LOG_INFO("Received Ping frame, sending Pong response");
            auto pong_result = co_await writer.sendPong(message);
            if (!pong_result) {
                HTTP_LOG_ERROR("Failed to send Pong: {}", pong_result.error().message());
                break;
            }
            HTTP_LOG_INFO("Pong sent successfully");
        }
        else if (opcode == WsOpcode::Pong) {
            // 收到 Pong 响应
            HTTP_LOG_INFO("Received Pong frame");
        }
        else if (opcode == WsOpcode::Close) {
            // 收到关闭请求
            HTTP_LOG_INFO("Received Close frame, closing connection");
            co_await writer.sendClose();
            break;
        }
        else if (opcode == WsOpcode::Text || opcode == WsOpcode::Binary) {
            // 处理数据消息
            HTTP_LOG_INFO("Received {} message: {}",
                    opcode == WsOpcode::Text ? "text" : "binary",
                    message.substr(0, std::min(message.size(), size_t(100))));

            // 回显消息
            std::string echo_msg = "Echo: " + message;
            auto echo_result = co_await writer.sendText(echo_msg);
            if (!echo_result) {
                HTTP_LOG_ERROR("Failed to send echo message: {}", echo_result.error().message());
                break;
            }
        }
    }

    // 关闭连接
    HTTP_LOG_INFO("Closing WebSocket connection");
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
        HTTP_LOG_ERROR("Failed to read HTTP request: {}", read_result.error().message());
        co_await conn.close();
        co_return;
    }

    HTTP_LOG_INFO("Received {} {}", httpMethodToString(request.header().method()), request.header().uri());

    // 检查是否是 WebSocket 升级请求
    if (request.header().uri() == "/ws") {
        // 处理 WebSocket 升级
        auto upgrade_result = WsUpgrade::handleUpgrade(request);

        if (!upgrade_result.success) {
            HTTP_LOG_ERROR("WebSocket upgrade failed: {}", upgrade_result.error_message);

            // 发送错误响应
            auto writer = conn.getWriter();
            co_await writer.sendResponse(upgrade_result.response);
            co_await conn.close();
            co_return;
        }

        HTTP_LOG_INFO("WebSocket upgrade successful");

        // 发送 101 Switching Protocols 响应
        auto writer = conn.getWriter();
        auto send_result = co_await writer.sendResponse(upgrade_result.response);

        if (!send_result) {
            HTTP_LOG_ERROR("Failed to send upgrade response: {}", send_result.error().message());
            co_await conn.close();
            co_return;
        }

        // 升级到 WebSocket 连接
        WsReaderSetting reader_setting;
        reader_setting.max_frame_size = 1024 * 1024;  // 1MB
        reader_setting.max_message_size = 10 * 1024 * 1024;  // 10MB

        WsWriterSetting writer_setting;

        // 从 HttpConn 创建 WebSocket 连接（转移所有权）
        WsConn ws_conn(
            std::move(conn),
            reader_setting,
            writer_setting,
            true  // is_server
        );

        // 处理 WebSocket 连接（通过引用传递，避免移动导致引用失效）
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
    HTTP_LOG_INFO("========================================");
    HTTP_LOG_INFO("WebSocket Server Example");
    HTTP_LOG_INFO("========================================\n");

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
    HTTP_LOG_INFO("Starting WebSocket server on {}:{}", config.host, config.port);
    HTTP_LOG_INFO("WebSocket endpoint: ws://localhost:8080/ws");
    HTTP_LOG_INFO("HTTP endpoint: http://localhost:8080/");
    HTTP_LOG_INFO("Press Ctrl+C to stop\n");

    // 启动服务器并传入处理器
    server.start(handleHttpRequest);

    // 保持服务器运行
    HTTP_LOG_INFO("Server is running. Press Ctrl+C to stop.");
    while (server.isRunning()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    HTTP_LOG_INFO("Server stopped");
    return 0;
#else
    HTTP_LOG_ERROR("No scheduler defined. Please compile with -DUSE_KQUEUE, -DUSE_EPOLL, or -DUSE_IOURING");
    return 1;
#endif
}
