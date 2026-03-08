/**
 * @file example_websocket_server.cc
 * @brief WebSocket 服务器完整示例
 * @details 展示如何使用 WsUpgrade 和手动处理控制帧（Ping/Pong/Close）
 */

#include <chrono>
#include "galay-http/kernel/http/HttpServer.h"
#include "galay-http/kernel/websocket/WsUpgrade.h"
#include "galay-http/kernel/websocket/WsConn.h"
#include "galay-http/kernel/websocket/WsWriterSetting.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/utils/Http1_1ResponseBuilder.h"
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
    HTTP_LOG_INFO("[ws] [conn] [open]");


     // 升级到 WebSocket 连接
     WsReaderSetting reader_setting;
     reader_setting.max_frame_size = 1024 * 1024;  // 1MB
     reader_setting.max_message_size = 10 * 1024 * 1024;  // 10MB

    // 获取 Reader 和 Writer（必须在协程开始时获取，保证 ws_conn 生命周期）
    auto reader = ws_conn.getReader(reader_setting);
    auto writer = ws_conn.getWriter(WsWriterSetting::byServer());

    // 发送欢迎消息
    HTTP_LOG_INFO("[ws] [welcome] [send]");
    auto send_result = co_await writer.sendText("Welcome to WebSocket server!");
    if (!send_result) {
        HTTP_LOG_ERROR("[ws] [welcome] [send-fail] [{}]", send_result.error().message());
        co_return;
    }
    HTTP_LOG_INFO("[ws] [welcome] [sent]");

    // 消息循环
    HTTP_LOG_INFO("[ws] [loop] [start]");
    while (true) {
        std::string message;
        WsOpcode opcode;

        // 读取消息（包括数据帧和控制帧）
        auto result = co_await reader.getMessage(message, opcode).timeout(1000ms);

        if (!result.has_value()) {
            WsError error = result.error();
            if (error.code() == kWsConnectionClosed) {
                HTTP_LOG_INFO("[ws] [conn] [closed]");
                break;
            }
            HTTP_LOG_ERROR("[ws] [recv-fail] [{}]", error.message());
            break;
        }

        if (!result.value()) {
            // 消息不完整，继续读取
            continue;
        }

        // 根据 opcode 判断消息类型并处理
        if (opcode == WsOpcode::Ping) {
            // 收到 Ping，发送 Pong 响应
            HTTP_LOG_INFO("[ws] [ping] [recv] [pong] [send]");
            auto pong_result = co_await writer.sendPong(message);
            if (!pong_result) {
                HTTP_LOG_ERROR("[ws] [pong] [send-fail] [{}]", pong_result.error().message());
                break;
            }
            HTTP_LOG_INFO("[ws] [pong] [sent]");
        }
        else if (opcode == WsOpcode::Pong) {
            // 收到 Pong 响应
            HTTP_LOG_INFO("[ws] [pong] [recv]");
        }
        else if (opcode == WsOpcode::Close) {
            // 收到关闭请求
            HTTP_LOG_INFO("[ws] [close] [recv]");
            auto close_result = co_await writer.sendClose();
            if (!close_result) {
                HTTP_LOG_ERROR("[ws] [close] [send-fail] [{}]", close_result.error().message());
            }
            break;
        }
        else if (opcode == WsOpcode::Text || opcode == WsOpcode::Binary) {
            // 处理数据消息
            HTTP_LOG_INFO("[ws] [recv] [type={}] [msg={}]",
                    opcode == WsOpcode::Text ? "text" : "binary",
                    message.substr(0, std::min(message.size(), size_t(100))));

            // 回显消息
            std::string echo_msg = "Echo: " + message;
            auto echo_result = co_await writer.sendText(echo_msg);
            if (!echo_result) {
                HTTP_LOG_ERROR("[ws] [echo] [send-fail] [{}]", echo_result.error().message());
                break;
            }
        }
    }

    // 关闭连接
    HTTP_LOG_INFO("[ws] [conn] [close]");
    auto close_result = co_await ws_conn.close();
    if (!close_result) {
        HTTP_LOG_ERROR("[ws] [conn] [close-fail] [{}]", close_result.error().message());
    }
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
        HTTP_LOG_ERROR("[http] [req] [read-fail] [{}]", read_result.error().message());
        auto close_result = co_await conn.close();
        if (!close_result) {
            HTTP_LOG_ERROR("[http] [conn] [close-fail] [{}]", close_result.error().message());
        }
        co_return;
    }

    HTTP_LOG_INFO("[http] [req] [{}] [{}]", httpMethodToString(request.header().method()), request.header().uri());

    // 检查是否是 WebSocket 升级请求
    if (request.header().uri() == "/ws") {
        // 处理 WebSocket 升级
        auto upgrade_result = WsUpgrade::handleUpgrade(request);

        if (!upgrade_result.success) {
            HTTP_LOG_ERROR("[ws] [upgrade] [fail] [{}]", upgrade_result.error_message);

            // 发送错误响应
            auto writer = conn.getWriter();
            auto result = co_await writer.sendResponse(upgrade_result.response);
            if (!result) {
                HTTP_LOG_ERROR("[upgrade] [send-fail] [{}]", result.error().message());
            }
            auto close_result = co_await conn.close();
            if (!close_result) {
                HTTP_LOG_ERROR("[http] [conn] [close-fail] [{}]", close_result.error().message());
            }
            co_return;
        }

        HTTP_LOG_INFO("[ws] [upgrade] [ok]");

        // 发送 101 Switching Protocols 响应
        auto writer = conn.getWriter();
        auto send_result = co_await writer.sendResponse(upgrade_result.response);

        if (!send_result) {
            HTTP_LOG_ERROR("[ws] [upgrade] [send-fail] [{}]", send_result.error().message());
            auto close_result = co_await conn.close();
            if (!close_result) {
                HTTP_LOG_ERROR("[http] [conn] [close-fail] [{}]", close_result.error().message());
            }
            co_return;
        }

        // 从 HttpConn 创建 WebSocket 连接（转移所有权）
        WsConn ws_conn = WsConn::from(std::move(conn), true);

        // 处理 WebSocket 连接（通过引用传递，避免移动导致引用失效）
        co_await handleWebSocketConnection(ws_conn).wait();
        co_return;
    }

    // 普通 HTTP 请求
    auto response = Http1_1ResponseBuilder()
        .status(HttpStatusCode::OK_200)
        .header("Content-Type", "text/html; charset=utf-8")
        .buildMove();

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
    auto result = co_await writer.sendResponse(response);
    if (!result) {
        HTTP_LOG_ERROR("[send] [fail] [{}]", result.error().message());
    }
    auto close_result = co_await conn.close();
    if (!close_result) {
        HTTP_LOG_ERROR("[http] [conn] [close-fail] [{}]", close_result.error().message());
    }
    co_return;
}

// ==================== 主函数 ====================

int main() {
    HTTP_LOG_INFO("[example] [ws] [server]");

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
    // 配置服务器
    // 创建 HTTP 服务器
    HttpServer server(HttpServerBuilder()
        .host("0.0.0.0")
        .port(8080)
        .backlog(128)
        .ioSchedulerCount(4)
        .computeSchedulerCount(2)
        .build());

    // 启动服务器
    HTTP_LOG_INFO("[listen] [ws] [{}:{}]", "0.0.0.0", 8080);
    HTTP_LOG_INFO("[endpoint] [ws] [ws://localhost:8080/ws]");
    HTTP_LOG_INFO("[endpoint] [http] [http://localhost:8080/]");
    HTTP_LOG_INFO("[ctrl] [stop]");

    // 启动服务器并传入处理器
    server.start(handleHttpRequest);

    // 保持服务器运行
    HTTP_LOG_INFO("[server] [running]");
    while (server.isRunning()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    HTTP_LOG_INFO("[server] [stopped]");
    return 0;
#else
    HTTP_LOG_ERROR("[scheduler] [missing] [define] [-DUSE_KQUEUE|-DUSE_EPOLL|-DUSE_IOURING]");
    return 1;
#endif
}
