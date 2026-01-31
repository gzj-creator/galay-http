/**
 * @file E7-WssServer.cc
 * @brief WSS (WebSocket Secure) 服务器示例
 * @details 演示如何在 HttpsServer 基础上实现 WSS 服务器
 *
 * @note 当前 galay-ssl 的 SslSocket 不支持 readv，因此 WSS 需要使用
 *       HttpsServer + 手动 WebSocket 帧处理的方式实现。
 *       完整的 WSS 支持需要为 SslSocket 添加 readv 方法。
 */

#include <chrono>
#include <csignal>
#include <iostream>
#include "galay-http/kernel/http/HttpServer.h"
#include "galay-http/kernel/websocket/WsUpgrade.h"
#include "galay-http/protoc/websocket/WebSocketFrame.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/kernel/http/HttpLog.h"

#ifdef GALAY_HTTP_SSL_ENABLED

using namespace galay::http;
using namespace galay::websocket;
using namespace galay::kernel;
using namespace std::chrono_literals;

static std::atomic<bool> g_running{true};
static std::atomic<uint64_t> g_connections{0};
static std::atomic<uint64_t> g_messages{0};

void signalHandler(int) {
    g_running = false;
}

/**
 * @brief 处理 WSS 连接（使用底层帧处理）
 * @details 由于 SslSocket 不支持 readv，这里使用 recv 直接读取数据
 */
Coroutine handleWssConnection(galay::ssl::SslSocket& socket) {
    HTTP_LOG_INFO("WSS connection established");
    g_connections++;

    // 发送欢迎消息
    WsFrame welcome_frame = WsFrameParser::createTextFrame("Welcome to WSS server!");
    std::string welcome_data = WsFrameParser::toBytes(welcome_frame, false);

    size_t sent = 0;
    while (sent < welcome_data.size()) {
        auto result = co_await socket.send(welcome_data.data() + sent, welcome_data.size() - sent);
        if (!result) {
            HTTP_LOG_ERROR("Failed to send welcome: {}", result.error().message());
            g_connections--;
            co_return;
        }
        sent += result.value();
    }

    // 接收缓冲区
    std::vector<char> buffer(8192);
    std::string accumulated;

    // 消息循环
    while (true) {
        auto recv_result = co_await socket.recv(buffer.data(), buffer.size());
        if (!recv_result) {
            HTTP_LOG_DEBUG("Recv error: {}", recv_result.error().message());
            break;
        }

        size_t bytes_received = recv_result.value().size();
        if (bytes_received == 0) {
            HTTP_LOG_INFO("Connection closed by peer");
            break;
        }

        accumulated.append(buffer.data(), bytes_received);

        // 尝试解析帧
        while (!accumulated.empty()) {
            WsFrame frame;
            std::vector<iovec> iovecs;
            iovecs.push_back({const_cast<char*>(accumulated.data()), accumulated.size()});

            auto parse_result = WsFrameParser::fromIOVec(iovecs, frame, true);
            if (!parse_result) {
                if (parse_result.error().code() == kWsIncomplete) {
                    break;  // 需要更多数据
                }
                HTTP_LOG_ERROR("Frame parse error: {}", parse_result.error().message());
                goto cleanup;
            }

            size_t consumed = parse_result.value();
            accumulated.erase(0, consumed);

            // 处理帧
            if (frame.header.opcode == WsOpcode::Close) {
                HTTP_LOG_INFO("Received Close frame");
                WsFrame close_frame = WsFrameParser::createCloseFrame(WsCloseCode::Normal);
                std::string close_data = WsFrameParser::toBytes(close_frame, false);
                co_await socket.send(close_data.data(), close_data.size());
                goto cleanup;
            }
            else if (frame.header.opcode == WsOpcode::Ping) {
                HTTP_LOG_DEBUG("Received Ping, sending Pong");
                WsFrame pong_frame = WsFrameParser::createPongFrame(frame.payload);
                std::string pong_data = WsFrameParser::toBytes(pong_frame, false);
                co_await socket.send(pong_data.data(), pong_data.size());
            }
            else if (frame.header.opcode == WsOpcode::Text || frame.header.opcode == WsOpcode::Binary) {
                g_messages++;
                HTTP_LOG_DEBUG("Received message: {}", frame.payload.substr(0, 50));

                // 回显
                std::string echo = "Echo: " + frame.payload;
                WsFrame echo_frame = WsFrameParser::createTextFrame(echo);
                std::string echo_data = WsFrameParser::toBytes(echo_frame, false);

                size_t echo_sent = 0;
                while (echo_sent < echo_data.size()) {
                    auto r = co_await socket.send(echo_data.data() + echo_sent, echo_data.size() - echo_sent);
                    if (!r) break;
                    echo_sent += r.value();
                }
            }
        }
    }

cleanup:
    co_await socket.close();
    g_connections--;
    HTTP_LOG_INFO("WSS connection closed");
    co_return;
}

/**
 * @brief HTTPS 请求处理器（处理 WSS 升级）
 */
Coroutine httpsHandler(HttpConnImpl<galay::ssl::SslSocket> conn) {
    HTTP_LOG_DEBUG("httpsHandler started");
    auto reader = conn.getReader();
    HttpRequest request;

    // 读取请求
    while (true) {
        auto r = co_await reader.getRequest(request);
        if (!r) {
            HTTP_LOG_ERROR("Failed to read request: {}", r.error().message());
            co_await conn.close();
            co_return;
        }
        if (r.value()) break;
    }

    HTTP_LOG_DEBUG("Received {} {}", httpMethodToString(request.header().method()), request.header().uri());

    // 检查是否是 WebSocket 升级请求
    std::string uri = request.header().uri();
    if (uri == "/ws" || uri.starts_with("/ws?")) {
        auto upgrade_result = WsUpgrade::handleUpgrade(request);

        if (!upgrade_result.success) {
            HTTP_LOG_ERROR("WSS upgrade failed: {}", upgrade_result.error_message);
            auto writer = conn.getWriter();
            while (true) {
                auto r = co_await writer.sendResponse(upgrade_result.response);
                if (!r || r.value()) break;
            }
            co_await conn.close();
            co_return;
        }

        HTTP_LOG_INFO("WSS upgrade successful");

        // 发送 101 Switching Protocols
        auto writer = conn.getWriter();
        HTTP_LOG_DEBUG("Sending 101 Switching Protocols response");
        while (true) {
            auto r = co_await writer.sendResponse(upgrade_result.response);
            if (!r) {
                HTTP_LOG_ERROR("Failed to send upgrade response: {}", r.error().message());
                co_await conn.close();
                co_return;
            }
            if (r.value()) break;
        }
        HTTP_LOG_DEBUG("101 response sent successfully");

        // 获取底层 socket 并处理 WebSocket 连接
        // 注意：这里需要直接访问 socket，因为 WsConn 模板不支持 SslSocket
        auto& socket = conn.getSocket();
        co_await handleWssConnection(socket).wait();
        co_return;
    }

    // 普通 HTTPS 请求 - 返回测试页面
    HttpResponse response;
    response.header().version() = HttpVersion::HttpVersion_1_1;
    response.header().code() = HttpStatusCode::OK_200;
    response.header().headerPairs().addHeaderPair("Content-Type", "text/html; charset=utf-8");

    std::string body = R"html(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>WSS Server Example</title>
    <style>
        body { font-family: Arial, sans-serif; max-width: 800px; margin: 50px auto; padding: 20px; }
        pre { background: #f4f4f4; padding: 15px; border-radius: 5px; overflow-x: auto; }
        .log { height: 300px; overflow-y: auto; border: 1px solid #ccc; padding: 10px; margin: 10px 0; font-family: monospace; }
        input { width: 70%; padding: 8px; }
        button { padding: 8px 16px; margin: 2px; }
    </style>
</head>
<body>
    <h1>WSS (WebSocket Secure) Server</h1>
    <p>Connect to: <code>wss://localhost:8443/ws</code></p>

    <h2>Interactive Test</h2>
    <div>
        <input type="text" id="message" placeholder="Enter message..." value="Hello WSS!">
        <button id="sendBtn">Send</button>
        <button id="connectBtn">Connect</button>
        <button id="disconnectBtn">Disconnect</button>
    </div>
    <div class="log" id="log"></div>

    <h2>Command Line Test</h2>
    <pre>
websocat -k wss://localhost:8443/ws
wscat -n -c wss://localhost:8443/ws
    </pre>

    <script>
        var ws = null;
        var logEl = document.getElementById('log');
        var msgEl = document.getElementById('message');

        function addLog(msg) {
            var time = new Date().toLocaleTimeString();
            logEl.innerHTML += '[' + time + '] ' + msg + '\n';
            logEl.scrollTop = logEl.scrollHeight;
        }

        document.getElementById('connectBtn').onclick = function() {
            if (ws && ws.readyState === WebSocket.OPEN) {
                addLog('Already connected');
                return;
            }
            ws = new WebSocket('wss://' + location.host + '/ws');
            ws.onopen = function() { addLog('Connected'); };
            ws.onmessage = function(e) { addLog('Received: ' + e.data); };
            ws.onerror = function(e) { addLog('Error occurred'); };
            ws.onclose = function() { addLog('Disconnected'); };
        };

        document.getElementById('disconnectBtn').onclick = function() {
            if (ws) {
                ws.close();
                ws = null;
            }
        };

        document.getElementById('sendBtn').onclick = function() {
            if (!ws || ws.readyState !== WebSocket.OPEN) {
                addLog('Not connected');
                return;
            }
            var msg = msgEl.value;
            ws.send(msg);
            addLog('Sent: ' + msg);
        };

        // Auto-connect
        document.getElementById('connectBtn').click();
    </script>
</body>
</html>)html";

    response.header().headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));
    response.setBodyStr(std::move(body));

    auto writer = conn.getWriter();
    while (true) {
        auto r = co_await writer.sendResponse(response);
        if (!r || r.value()) break;
    }
    co_await conn.close();
    co_return;
}

int main(int argc, char* argv[]) {
    int port = 8443;
    std::string cert_path = "test.crt";
    std::string key_path = "test.key";

    if (argc > 1) port = std::atoi(argv[1]);
    if (argc > 2) cert_path = argv[2];
    if (argc > 3) key_path = argv[3];

    std::cout << "========================================\n";
    std::cout << "WSS (WebSocket Secure) Server Example\n";
    std::cout << "========================================\n";

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try {
        HttpsServerConfig config;
        config.host = "0.0.0.0";
        config.port = port;
        config.cert_path = cert_path;
        config.key_path = key_path;
        config.io_scheduler_count = 4;

        HttpsServer server(config);

        std::cout << "Server running on https://0.0.0.0:" << port << "\n";
        std::cout << "WSS endpoint: wss://localhost:" << port << "/ws\n";
        std::cout << "Test page: https://localhost:" << port << "/\n";
        std::cout << "Press Ctrl+C to stop\n";
        std::cout << "========================================\n";

        server.start(httpsHandler);

        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        std::cout << "\nStatistics:\n";
        std::cout << "  Active connections: " << g_connections << "\n";
        std::cout << "  Total messages: " << g_messages << "\n";

        server.stop();
        std::cout << "Server stopped.\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

#else

int main() {
    std::cout << "SSL support is not enabled.\n";
    std::cout << "Rebuild with -DGALAY_HTTP_ENABLE_SSL=ON\n";
    return 0;
}

#endif
