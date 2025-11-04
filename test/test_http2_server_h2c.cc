// ========== 调试开关 ==========
// 取消注释下面这行可以启用所有 debug 日志
// #define ENABLE_DEBUG
// ==================================

#include "galay/common/Error.h"
#include "galay/kernel/coroutine/AsyncWaiter.hpp"
#include "galay/kernel/runtime/Runtime.h"
#include "galay-http/kernel/http/HttpRouter.h"
#include "galay-http/protoc/http/HttpBase.h"
#include "galay-http/server/HttpServer.h"
#include "galay-http/utils/HttpLogger.h"
#include "galay-http/utils/Http2DebugLog.h"
#include "galay-http/utils/HttpUtils.h"
#include "galay-http/kernel/http2/Http2Connection.h"
#include "galay-http/kernel/http2/Http2Params.hpp"
#include "galay-http/protoc/http2/Http2Hpack.h"
#include <csignal>
#include <galay/utils/SignalHandler.hpp>

using namespace galay;
using namespace galay::http;

/**
 * @brief HTTP/2 测试服务器
 * 
 * 类似于 WebSocket，使用 upgradeToHttp2 接口进行协议升级
 */

// HTTP/2 处理函数
// is_prior_knowledge: true 表示 Prior Knowledge h2c（直接发送 PRI），false 表示 Upgrade h2c
Coroutine<nil> handleHttp2(Http2Connection http2Conn, AsyncWaiter<void, Infallible>& waiter, Http2Settings settings, bool is_prior_knowledge = true)
{
    HTTP2_LOG_INFO("[HTTP/2] ======== HTTP/2 connection established ========");
    HTTP2_LOG_INFO("[HTTP/2] Mode: {}", is_prior_knowledge ? "Prior Knowledge" : "Upgrade");
    HTTP2_LOG_DEBUG("[HTTP/2] Connection isClosed: {}", http2Conn.isClosed());
    
    int frame_count = 0;
    
    // 创建 Reader 和 Writer
    HTTP2_LOG_DEBUG("[HTTP/2] Creating Reader and Writer...");
    auto reader = http2Conn.getReader(settings);
    auto writer = http2Conn.getWriter(settings);
    HTTP2_LOG_INFO("[HTTP/2] Reader and Writer created successfully");
    
    try {
        // 发送服务器的 SETTINGS 帧
        HTTP2_LOG_INFO("[HTTP/2] Sending server SETTINGS...");
        HTTP2_LOG_DEBUG("[HTTP/2]   max_concurrent_streams: {}", settings.max_concurrent_streams);
        HTTP2_LOG_DEBUG("[HTTP/2]   initial_window_size: {}", settings.initial_window_size);
        HTTP2_LOG_DEBUG("[HTTP/2]   max_frame_size: {}", settings.max_frame_size);
        auto settings_result = co_await writer.sendSettings(settings);
        if (!settings_result.has_value()) {
            HTTP2_LOG_ERROR("[HTTP/2] Failed to send SETTINGS: {}", settings_result.error().message());
            goto cleanup;
        }
        HTTP2_LOG_INFO("[HTTP/2] Server SETTINGS sent");
        
        // 根据模式决定是否读取前言
        if (is_prior_knowledge) {
            // Prior Knowledge: HttpReader 已经读取并验证了 PRI 前言
            // HttpReader 的 buffer 中还有客户端的 SETTINGS 帧，但 Http2Reader 无法访问
            // 我们直接进入帧处理循环，让 Http2Reader 从 socket 读取后续帧
            HTTP2_LOG_INFO("[HTTP/2] Prior Knowledge mode - PRI already validated by HttpReader");
            HTTP2_LOG_INFO("[HTTP/2] Note: HttpReader's buffer contains client SETTINGS, skipping initial response");
        } else {
            // Upgrade: 需要读取客户端的 PRI 前言
            HTTP2_LOG_INFO("[HTTP/2] Upgrade mode - waiting for client preface...");
            auto preface_result = co_await reader.readPreface();
            if (!preface_result.has_value()) {
                HTTP2_LOG_ERROR("[HTTP/2] Failed to read preface: {}", preface_result.error().message());
                goto cleanup;
            }
            HTTP2_LOG_INFO("[HTTP/2] Client preface received");
            
            // 在 Upgrade 模式下，为初始请求（stream 1）创建响应
            HTTP2_LOG_INFO("[HTTP/2] Creating stream 1 for initial request");
            auto stream1 = http2Conn.streamManager().createStream(1);
            if (!stream1) {
                HTTP2_LOG_ERROR("[HTTP/2] Failed to create stream 1");
                goto cleanup;
            }
            HTTP2_LOG_DEBUG("[HTTP/2] Stream 1 created successfully");
            
            // 发送对 stream 1 的响应
            HTTP2_LOG_INFO("[HTTP/2] Sending response to stream 1");
            
            // 使用 HPACK 编码头部
            std::string body = "Hello from HTTP/2!";
            HpackEncoder encoder;
            std::vector<HpackHeaderField> response_headers = {
                {":status", "200"},
                {"content-type", "text/plain; charset=utf-8"},
                {"content-length", std::to_string(body.size())},
                {"server", "galay-http2/0.1"}
            };
            std::string encoded_headers = encoder.encodeHeaders(response_headers);
            
            // 发送 HEADERS 帧
            auto headers_result = co_await writer.sendHeaders(1, encoded_headers, false, true);
            if (!headers_result.has_value()) {
                HTTP2_LOG_ERROR("[HTTP/2] Failed to send HEADERS: {}", headers_result.error().message());
                goto cleanup;
            }
            HTTP2_LOG_INFO("[HTTP/2] HEADERS sent for stream 1");
            
            // 发送 DATA 帧
            auto data_result = co_await writer.sendData(1, body, true);
            if (!data_result.has_value()) {
                HTTP2_LOG_ERROR("[HTTP/2] Failed to send DATA: {}", data_result.error().message());
                goto cleanup;
            }
            HTTP2_LOG_INFO("[HTTP/2] DATA sent for stream 1, response complete");
        }
        
        // 主循环：处理帧
        while (!http2Conn.isClosed()) {
            frame_count++;
            HTTP2_LOG_DEBUG("[HTTP/2] -------- Frame {} --------", frame_count);
            
            // 读取帧
            auto frame_result = co_await reader.readFrame();
            if (!frame_result.has_value()) {
                HTTP2_LOG_ERROR("[HTTP/2] Failed to read frame: {}", frame_result.error().message());
                break;
            }
            
            auto frame = frame_result.value();
            HTTP2_LOG_INFO("[HTTP/2] Received frame: type={}, stream={}, length={}, flags=0x{:02x}",
                          http2FrameTypeToString(frame->header().type),
                          frame->header().stream_id,
                          frame->header().length,
                          frame->header().flags);
            
            // 处理不同类型的帧
            switch (frame->header().type) {
                case Http2FrameType::SETTINGS: {
                    HTTP2_LOG_INFO("[HTTP/2] Processing SETTINGS frame");
                    auto settings_frame = std::dynamic_pointer_cast<Http2SettingsFrame>(frame);
                    
                    if (settings_frame->isAck()) {
                        HTTP2_LOG_INFO("[HTTP/2] Received SETTINGS ACK");
                    } else {
                        HTTP2_LOG_INFO("[HTTP/2] Received SETTINGS with {} parameters, sending ACK", 
                                      settings_frame->settings().size());
                        // 应用客户端的设置
                        for (const auto& [id, value] : settings_frame->settings()) {
                            HTTP2_LOG_DEBUG("[HTTP/2]   Setting {}: {}", static_cast<uint16_t>(id), value);
                        }
                        auto ack_result = co_await writer.sendSettingsAck();
                        if (!ack_result.has_value()) {
                            HTTP2_LOG_ERROR("[HTTP/2] Failed to send SETTINGS ACK");
                        } else {
                            HTTP2_LOG_INFO("[HTTP/2] SETTINGS ACK sent");
                        }
                    }
                    break;
                }
                
                case Http2FrameType::PING: {
                    HTTP2_LOG_INFO("[HTTP/2] Processing PING frame");
                    auto ping_frame = std::dynamic_pointer_cast<Http2PingFrame>(frame);
                    
                    if (!ping_frame->isAck()) {
                        HTTP2_LOG_INFO("[HTTP/2] Received PING, sending PONG");
                        co_await writer.sendPing(ping_frame->data(), true);
                    } else {
                        HTTP2_LOG_INFO("[HTTP/2] Received PONG");
                    }
                    break;
                }
                
                case Http2FrameType::HEADERS: {
                    HTTP2_LOG_INFO("[HTTP/2] Processing HEADERS frame on stream {}", 
                                  frame->header().stream_id);
                    // TODO: 解码头部并处理请求
                    break;
                }
                
                case Http2FrameType::DATA: {
                    HTTP2_LOG_INFO("[HTTP/2] Processing DATA frame on stream {}", 
                                  frame->header().stream_id);
                    // TODO: 处理数据帧
                    break;
                }
                
                case Http2FrameType::GOAWAY: {
                    HTTP2_LOG_WARN("[HTTP/2] Received GOAWAY");
                    goto cleanup;
                }
                
                default:
                    HTTP2_LOG_WARN("[HTTP/2] Unhandled frame type: {}", 
                                  http2FrameTypeToString(frame->header().type));
                    break;
            }
        }
    } catch (const std::exception& e) {
        HTTP2_LOG_ERROR("[HTTP/2] Exception: {}", e.what());
    }
    
cleanup:
    HTTP2_LOG_INFO("[HTTP/2] ======== Closing connection ========");
    HTTP2_LOG_INFO("[HTTP/2] Total frames processed: {}", frame_count);
    waiter.notify({});
    co_return nil();
}

// HTTP 升级到 HTTP/2 的处理函数（类似 WebSocket）
Coroutine<nil> http2Upgrade(HttpRequest& request, HttpConnection& conn, HttpParams params)
{
    HTTP2_LOG_INFO("========================================");
    HTTP2_LOG_INFO("[HTTP] Upgrading to HTTP/2");
    HTTP2_LOG_DEBUG("[HTTP] Connection isClosed: {}", conn.isClosed());
    
    // 创建 Writer 并执行 HTTP/2 升级
    auto writer = conn.getResponseWriter({});
    HTTP2_LOG_DEBUG("[HTTP] Writer created, starting upgrade...");
    
    auto upgrade_result = co_await writer.upgradeToHttp2(request);
    
    if (!upgrade_result.has_value()) {
        HTTP2_LOG_ERROR("[HTTP] Upgrade failed: {}", upgrade_result.error().message());
        auto response = HttpUtils::defaultBadRequest();
        co_await writer.reply(response);
        co_await conn.close();
        co_return nil();
    }
    
    HTTP2_LOG_INFO("[HTTP] Upgrade successful, switching to HTTP/2");
    
    // 创建 HTTP/2 连接
    HTTP2_LOG_DEBUG("[HTTP] Creating Http2Connection...");
    Http2Connection http2Conn = Http2Connection::from(conn);
    HTTP2_LOG_DEBUG("[HTTP] Http2Connection created");
    
    Http2Settings settings;
    settings.max_concurrent_streams = 100;
    settings.initial_window_size = 65535;
    settings.recv_timeout = std::chrono::milliseconds(30000);
    settings.send_timeout = std::chrono::milliseconds(30000);
    
    AsyncWaiter<void, Infallible> waiter;
    // 处理 HTTP/2 通信，传递 is_prior_knowledge=false (Upgrade 模式)
    HTTP2_LOG_DEBUG("[HTTP] Starting HTTP/2 handler task (Upgrade mode)...");
    waiter.appendTask(handleHttp2(std::move(http2Conn), waiter, settings, false));  // false = Upgrade mode
    co_await waiter.wait();
    HTTP2_LOG_INFO("[HTTP] HTTP/2 handler finished, waiter done");
    
    // HTTP/2 处理完成后，主动关闭连接
    HTTP2_LOG_DEBUG("[HTTP] Closing connection...");
    co_await conn.close();
    HTTP2_LOG_INFO("========================================");
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
    <title>HTTP/2 Test Server</title>
</head>
<body>
    <h1>HTTP/2 Test Server</h1>
    <p>Available HTTP/2 endpoints:</p>
    <ul>
        <li>http://localhost:8080/h2 - HTTP/2 upgrade endpoint</li>
        <li>http://localhost:8080/api/test - HTTP/2 test endpoint</li>
    </ul>
    <p>Use curl to test:</p>
    <pre>curl --http2-prior-knowledge http://localhost:8080/h2</pre>
</body>
</html>
)";
    auto response = HttpUtils::defaultOk("html", std::move(html));
    co_await writer.reply(response);
    co_await conn.close();
    co_return nil();
}

// Prior Knowledge h2c 处理函数（直接发送 PRI）
Coroutine<nil> http2PriorKnowledge(HttpRequest& request, HttpConnection& conn, HttpParams params)
{
    HTTP2_LOG_INFO("========================================");
    HTTP2_LOG_INFO("[HTTP] Prior Knowledge h2c detected (direct PRI)");
    HTTP2_LOG_INFO("[HTTP] Method: PRI, URI: *");
    
    // 创建 HTTP/2 连接
    HTTP2_LOG_DEBUG("[HTTP] Creating Http2Connection...");
    Http2Connection http2Conn = Http2Connection::from(conn);
    HTTP2_LOG_DEBUG("[HTTP] Http2Connection created");
    
    Http2Settings settings;
    settings.max_concurrent_streams = 100;
    settings.initial_window_size = 65535;
    settings.recv_timeout = std::chrono::milliseconds(30000);
    settings.send_timeout = std::chrono::milliseconds(30000);
    
    AsyncWaiter<void, Infallible> waiter;
    // 处理 HTTP/2 通信，传递 is_prior_knowledge=true
    HTTP2_LOG_DEBUG("[HTTP] Starting HTTP/2 handler task (Prior Knowledge mode)...");
    waiter.appendTask(handleHttp2(std::move(http2Conn), waiter, settings, true));  // true = Prior Knowledge
    co_await waiter.wait();
    HTTP2_LOG_INFO("[HTTP] HTTP/2 handler finished, waiter done");
    
    // HTTP/2 处理完成后，主动关闭连接
    HTTP2_LOG_DEBUG("[HTTP] Closing connection...");
    co_await conn.close();
    HTTP2_LOG_INFO("========================================");
    co_return nil();
}

HttpRouteMap map = {
    {"/", {httpIndex}},
    {"/h2", {http2Upgrade}},
    {"*", {http2PriorKnowledge}},  // 处理 PRI * HTTP/2.0 (Prior Knowledge)
};

int main()
{
    HTTP2_LOG_INFO("========================================");
    HTTP2_LOG_INFO("     HTTP/2 测试服务器");
    HTTP2_LOG_INFO("========================================");
    HTTP2_LOG_INFO("监听地址: http://localhost:8080");
    HTTP2_LOG_INFO("HTTP/2 endpoints:");
    HTTP2_LOG_INFO("  - http://localhost:8080/h2");
    HTTP2_LOG_INFO("  - http://localhost:8080/api/test");
    HTTP2_LOG_INFO("按 Ctrl+C 停止服务器");
    HTTP2_LOG_INFO("========================================");
    
    #ifdef ENABLE_DEBUG
    HTTP2_LOG_DEBUG("调试模式已启用 (ENABLE_DEBUG)");
    #else
    HTTP2_LOG_INFO("发布模式");
    #endif
    
    // 设置日志级别为 debug 以查看详细日志
    HttpLogger::getInstance()->getLogger()->getSpdlogger()->set_level(spdlog::level::level_enum::debug);
    HTTP2_LOG_DEBUG("Log level set to DEBUG");
    
    RuntimeBuilder runtimebuilder;
    auto runtime = runtimebuilder.build();
    runtime.start();
    
    HttpServerBuilder builder;
    HttpServer server = builder.build();
    server.listen(Host("0.0.0.0", 8080));
    
    utils::SignalHandler::setSignalHandler<SIGINT>([&server](int signal) {
        HTTP2_LOG_INFO("接收到停止信号 ({}), 关闭服务器", signal);
        server.stop();
    });
    
    HttpRouter router;
    router.addRoute<PRI>("*", http2PriorKnowledge);
    router.addRoute<GET>(map);
    
    HTTP2_LOG_INFO("服务器启动中...");
    server.run(runtime, router);
    server.wait();
    
    HTTP2_LOG_INFO("服务器已停止");
    return 0;
}
