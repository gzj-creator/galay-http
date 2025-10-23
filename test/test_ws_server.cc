// ========== 调试开关 ==========
// 取消注释下面这行可以启用所有 debug 日志
// 注意：启用后会严重影响性能！仅用于诊断问题
// #define ENABLE_DEBUG
// ==================================

#include "galay/common/Error.h"
#include "galay/kernel/coroutine/AsyncWaiter.hpp"
#include "galay/kernel/runtime/Runtime.h"
#include "galay-http/kernel/http/HttpRouter.h"
#include "galay-http/protoc/http/HttpBase.h"
#include "galay-http/server/HttpServer.h"
#include "galay-http/utils/HttpLogger.h"
#include "galay-http/utils/HttpUtils.h"
#include "galay-http/kernel/websocket/WsConnection.h"
#include "galay-http/kernel/websocket/WsParams.hpp"
#include <csignal>
#include <galay/utils/SignalHandler.hpp>

using namespace galay;
using namespace galay::http;

// WebSocket Echo 服务 - 回显收到的消息
Coroutine<nil> handleWebSocketEcho(WsConnection wsConn, AsyncWaiter<void, Infallible>& waiter, WsSettings settings)
{
    std::cout << "[WS Echo] ======== WebSocket connection established ========" << std::endl;
    std::cout << "[WS Echo] Connection isClosed: " << wsConn.isClosed() << std::endl;
    
    // 创建 Reader 和 Writer（作为局部变量，生命周期在整个函数内）
    std::cout << "[WS Echo] Creating Reader..." << std::endl;
    auto reader = wsConn.getReader(settings);
    std::cout << "[WS Echo] Creating Writer..." << std::endl;
    auto writer = wsConn.getWriter(settings);
    std::cout << "[WS Echo] Reader and Writer created successfully" << std::endl;
    
    int frame_count = 0;
    try {
        while (!wsConn.isClosed()) {
            frame_count++;
            std::cout << "[WS Echo] -------- Loop iteration " << frame_count << " --------" << std::endl;
            std::cout << "[WS Echo] Connection closed: " << wsConn.isClosed() << std::endl;
            std::cout << "[WS Echo] Calling readFrame()..." << std::endl;
            
            // 接收帧
            auto frame_result = co_await reader.readFrame();
            std::cout << "[WS Echo] readFrame() returned, has_value: " << frame_result.has_value() << std::endl;
            
            if (!frame_result.has_value()) {
                std::cout << "[WS Echo] Receive error: " << frame_result.error().message() << std::endl;
                break;
            }
            
            auto& frame = frame_result.value();
            
            // 处理不同类型的帧
            switch (frame.opcode()) {
                case WsOpcode::Text:
                    std::cout << "[WS Echo] Received text: " << frame.payload() << std::endl;
                    
                    // 如果收到 "SEND_PING" 命令，服务器主动发送 Ping
                    if (frame.payload() == "SEND_PING") {
                        std::cout << "[WS Echo] Server sending Ping to client..." << std::endl;
                        auto ping_result = co_await writer.sendPing("server-ping");
                        if (ping_result.has_value()) {
                            std::cout << "[WS Echo] Ping sent successfully, client should auto-reply Pong" << std::endl;
                        } else {
                            std::cout << "[WS Echo] Failed to send Ping: " << ping_result.error().message() << std::endl;
                        }
                        break;
                    }
                    
                    // 回显文本消息
                    co_await writer.sendText(frame.payload());
                    break;
                    
                case WsOpcode::Binary:
                    std::cout << "[WS Echo] Received binary data: " << frame.payload().size() << " bytes" << std::endl;
                    // 回显二进制消息
                    co_await writer.sendBinary(frame.payload());
                    break;
                    
                case WsOpcode::Ping:
                    std::cout << "[WS Echo] Received Ping" << std::endl;
                    // 自动回复 Pong
                    co_await writer.sendPong(frame.payload());
                    break;
                    
                case WsOpcode::Pong:
                    std::cout << "[WS Echo] Received Pong" << std::endl;
                    break;
                    
                case WsOpcode::Close:
                    std::cout << "[WS Echo] Received Close frame" << std::endl;
                    // 回复关闭帧
                    co_await writer.sendClose(WsCloseCode::Normal, "Goodbye");
                    // 不要在这里 return，让它走到最后统一 notify
                    goto cleanup;
                    
                default:
                    std::cout << "[WS Echo] Unknown opcode" << std::endl;
                    break;
            }
        }
    } catch (const std::exception& e) {
        std::cout << "[WS Echo] Exception occurred: " << e.what() << std::endl;
    } catch (...) {
        std::cout << "[WS Echo] Unknown exception occurred" << std::endl;
    }
    
cleanup:
    std::cout << "[WS Echo] ======== Cleaning up and closing ========" << std::endl;
    std::cout << "[WS Echo] Total frames processed: " << frame_count << std::endl;
    std::cout << "[WS Echo] Connection closed" << std::endl;
    waiter.notify({});
    co_return nil();
}

// WebSocket 聊天服务 - 广播消息到所有连接的客户端
Coroutine<nil> handleWebSocketChat(WsConnection wsConn, AsyncWaiter<void, Infallible>& waiter, WsSettings settings)
{
    std::cout << "[WS Chat] WebSocket connection established" << std::endl;
    
    // 创建 Reader 和 Writer
    auto reader = wsConn.getReader(settings);
    auto writer = wsConn.getWriter(settings);
    
    // 发送欢迎消息
    co_await writer.sendText("Welcome to WebSocket Chat!");
    
    try {
        while (!wsConn.isClosed()) {
            // 使用 readFrame() 而不是 readMessage()，以便处理控制帧
            auto frame_result = co_await reader.readFrame();
            
            if (!frame_result.has_value()) {
                std::cout << "[WS Chat] Receive error: " << frame_result.error().message() << std::endl;
                break;
            }
            
            auto& frame = frame_result.value();
            
            // 处理不同类型的帧
            switch (frame.opcode()) {
                case WsOpcode::Text:
                    std::cout << "[WS Chat] Received text: " << frame.payload() << std::endl;
                    // 广播消息（这里简化为回显）
                    {
                        std::string broadcast = "Broadcast: " + frame.payload();
                        auto send_result = co_await writer.sendText(broadcast);
                        if (!send_result.has_value()) {
                            std::cout << "[WS Chat] Send error: " << send_result.error().message() << std::endl;
                            goto cleanup;
                        }
                    }
                    break;
                    
                case WsOpcode::Binary:
                    std::cout << "[WS Chat] Received binary data: " << frame.payload().size() << " bytes" << std::endl;
                    // 回显二进制消息
                    co_await writer.sendBinary(frame.payload());
                    break;
                    
                case WsOpcode::Ping:
                    std::cout << "[WS Chat] Received Ping" << std::endl;
                    // 自动回复 Pong
                    co_await writer.sendPong(frame.payload());
                    break;
                    
                case WsOpcode::Pong:
                    std::cout << "[WS Chat] Received Pong" << std::endl;
                    break;
                    
                case WsOpcode::Close:
                    std::cout << "[WS Chat] Received Close frame" << std::endl;
                    // 回复关闭帧
                    co_await writer.sendClose(WsCloseCode::Normal, "Goodbye");
                    goto cleanup;
                    
                default:
                    std::cout << "[WS Chat] Unknown opcode" << std::endl;
                    break;
            }
        }
    } catch (const std::exception& e) {
        std::cout << "[WS Chat] Exception occurred: " << e.what() << std::endl;
    } catch (...) {
        std::cout << "[WS Chat] Unknown exception occurred" << std::endl;
    }
    
cleanup:
    std::cout << "[WS Chat] Connection closed" << std::endl;
    waiter.notify({});
    co_return nil();
}

// HTTP 升级到 WebSocket 的处理函数
Coroutine<nil> wsEchoUpgrade(HttpRequest& request, HttpConnection& conn, HttpParams params)
{
    std::cout << "========================================" << std::endl;
    std::cout << "[HTTP] Upgrading to WebSocket (Echo)" << std::endl;
    std::cout << "[HTTP] Connection isClosed: " << conn.isClosed() << std::endl;
    
    // 创建 Writer 并执行 WebSocket 升级
    auto writer = conn.getResponseWriter({});
    std::cout << "[HTTP] Writer created, starting upgrade..." << std::endl;
    
    auto upgrade_result = co_await writer.upgradeToWebSocket(request);
    
    if (!upgrade_result.has_value()) {
        std::cout << "[HTTP] Upgrade failed: " << upgrade_result.error().message() << std::endl;
        auto response = HttpUtils::defaultBadRequest();
        co_await writer.reply(response);
        co_await conn.close();
        co_return nil();
    }
    
    std::cout << "[HTTP] Upgrade successful, switching to WebSocket" << std::endl;
    
    // 创建 WebSocket 连接
    std::cout << "[HTTP] Creating WsConnection..." << std::endl;
    WsConnection wsConn = WsConnection::from(conn);
    std::cout << "[HTTP] WsConnection created" << std::endl;
    
    WsSettings settings;
    settings.recv_timeout = std::chrono::milliseconds(30000);
    settings.send_timeout = std::chrono::milliseconds(30000);
    settings.auto_ping = true;
    settings.auto_pong = true;
    
    AsyncWaiter<void, Infallible> waiter;
    // 处理 WebSocket 通信
    std::cout << "[HTTP] Starting WebSocket handler task..." << std::endl;
    waiter.appendTask(handleWebSocketEcho(std::move(wsConn), waiter, settings));
    co_await waiter.wait();
    std::cout << "[HTTP] WebSocket handler finished, waiter done......." << std::endl;
    
    // WebSocket 处理完成后，主动关闭连接，避免进入下一次 HTTP 请求循环
    std::cout << "[HTTP] Closing connection..." << std::endl;
    co_await conn.close();
    std::cout << "========================================" << std::endl;
    co_return nil();
}

// HTTP 升级到 WebSocket 的处理函数（聊天服务）
Coroutine<nil> wsChatUpgrade(HttpRequest& request, HttpConnection& conn, HttpParams params)
{
    std::cout << "[HTTP] Upgrading to WebSocket (Chat)" << std::endl;
    
    // 创建 Writer 并执行 WebSocket 升级
    auto writer = conn.getResponseWriter({});
    auto upgrade_result = co_await writer.upgradeToWebSocket(request);
    
    if (!upgrade_result.has_value()) {
        std::cout << "[HTTP] Upgrade failed: " << upgrade_result.error().message() << std::endl;
        auto response = HttpUtils::defaultBadRequest();
        co_await writer.reply(response);
        co_await conn.close();
        co_return nil();
    }
    
    WsConnection wsConn = WsConnection::from(conn);
    WsSettings settings;
    settings.recv_timeout = std::chrono::milliseconds(30000);
    settings.send_timeout = std::chrono::milliseconds(30000);
    
    AsyncWaiter<void, Infallible> waiter;
    // 处理 WebSocket 通信
    waiter.appendTask(handleWebSocketChat(std::move(wsConn), waiter, settings));
    co_await waiter.wait();

    std::cout << "[HTTP] WebSocket handler finished, waiter done......." << std::endl;
    
    // WebSocket 处理完成后，主动关闭连接，避免进入下一次 HTTP 请求循环
    std::cout << "[HTTP] Closing connection..." << std::endl;
    co_await conn.close();
    co_return nil();
}

// 普通 HTTP 端点
Coroutine<nil> httpIndex(HttpRequest& request, HttpConnection& conn, HttpParams params)
{
    auto writer = conn.getResponseWriter({});
    std::string html = R"(
<!DOCTYPE html>
<html>
<head>
    <title>WebSocket Test Server</title>
</head>
<body>
    <h1>WebSocket Test Server</h1>
    <p>Available WebSocket endpoints:</p>
    <ul>
        <li>ws://localhost:8080/ws/echo - Echo server</li>
        <li>ws://localhost:8080/ws/chat - Chat server</li>
    </ul>
    <script>
        // Example WebSocket connection
        const ws = new WebSocket('ws://localhost:8080/ws/echo');
        ws.onopen = () => console.log('Connected');
        ws.onmessage = (e) => console.log('Received:', e.data);
        ws.onerror = (e) => console.error('Error:', e);
    </script>
</body>
</html>
)";
    auto response = HttpUtils::defaultOk("html", std::move(html));
    co_await writer.reply(response);
    co_await conn.close();
    co_return nil();
}

HttpRouteMap map = {
    {"/", {httpIndex}},
    {"/ws/echo", {wsEchoUpgrade}},
    {"/ws/chat", {wsChatUpgrade}}
};

int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << "Starting WebSocket Test Server..." << std::endl;
    std::cout << "Server will listen on 0.0.0.0:8080" << std::endl;
    std::cout << "WebSocket endpoints:" << std::endl;
    std::cout << "  - ws://localhost:8080/ws/echo (Echo service)" << std::endl;
    std::cout << "  - ws://localhost:8080/ws/chat (Chat service)" << std::endl;
    std::cout << "HTTP endpoint:" << std::endl;
    std::cout << "  - http://localhost:8080/ (Test page)" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    // 设置日志级别为 debug 以查看详细日志
    HttpLogger::getInstance()->getLogger()->getSpdlogger()->set_level(spdlog::level::level_enum::debug);
    std::cout << "[Main] Log level set to DEBUG" << std::endl;
    
    RuntimeBuilder runtimebuilder;
    auto runtime = runtimebuilder.build();
    runtime.start();
    
    HttpServerBuilder builder;
    HttpServer server = builder.build();
    server.listen(Host("0.0.0.0", 8080));
    
    utils::SignalHandler::setSignalHandler<SIGINT>([&server](int signal) {
        std::cout << "\nReceived signal: " << signal << ", shutting down..." << std::endl;
        server.stop();
    });
    
    HttpRouter router;
    router.addRoute<GET>(map);
    
    server.run(runtime, router);
    server.wait();
    
    std::cout << "Server stopped" << std::endl;
    return 0;
}

