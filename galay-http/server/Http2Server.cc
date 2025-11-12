#include "Http2Server.h"
#include "galay-http/kernel/http/HttpsConnection.h"
#include "galay-http/kernel/http/HttpsRouter.h"
#include "galay-http/kernel/http/HttpParams.hpp"
#include "galay-http/kernel/http2/Http2Reader.h"
#include "galay-http/kernel/http2/Http2Router.h"
#include "galay-http/kernel/http2/Http2Writer.h"
#include "galay-http/kernel/http/HttpsWriter.h"
#include "galay-http/kernel/http/HttpsReader.h"
#include "galay-http/protoc/http2/Http2Error.h"
#include "galay-http/protoc/http2/Http2Hpack.h"
#include "galay-http/protoc/alpn/AlpnProtocol.h"
#include "galay-http/utils/HttpUtils.h"
#include "galay-http/utils/Http2DebugLog.h"
#include "galay/kernel/coroutine/AsyncWaiter.hpp"
#include "galay/kernel/coroutine/CoSchedulerHandle.hpp"
#include <utility>

namespace galay::http
{
    void Http2Server::configureAlpn(bool with_fallback)
    {
        if (m_alpn_configured) {
            return;
        }
        
        // 初始化 SSL 上下文
        m_server.initializeSSLContext();
        SSL_CTX* ctx = m_server.getSSLContext();
        
        if (ctx) {
            AlpnProtocolList alpn_list;
            if (with_fallback) {
                // 配置 h2 + http/1.1（支持降级）
                alpn_list = AlpnProtocolList::http2WithFallback();
                HTTP2_LOG_INFO("[Http2Server] ALPN configured: h2, http/1.1 (with fallback)");
            } else {
                // 仅配置 h2（HTTP/2 only）
                alpn_list = AlpnProtocolList::http2Only();
                HTTP2_LOG_INFO("[Http2Server] ALPN configured: h2 only");
            }
            
            if (!configureServerAlpn(ctx, alpn_list)) {
                HTTP2_LOG_ERROR("[Http2Server] Failed to configure ALPN");
            }
            m_alpn_configured = true;
        } else {
            HTTP2_LOG_ERROR("[Http2Server] Cannot get SSL_CTX!");
        }
    }
    
    void Http2Server::listen(const Host& host)
    {
        HTTP2_LOG_DEBUG("[Http2Server] listen() called for {}:{}", host.ip, host.port);
        m_server.listenOn(host, DEFAULT_TCP_BACKLOG_SIZE);
        HTTP2_LOG_INFO("[Http2Server] Listening on {}:{}", host.ip, host.port);
    }
    
    void Http2Server::run(Runtime& runtime, const Http2Callbacks& callbacks, Http2Settings params)
    {
        HTTP2_LOG_INFO("[Http2Server] Starting HTTP/2 server (h2 only, no fallback)");
        
        // 配置 ALPN 为 h2 only
        configureAlpn(false);
        
        m_server.run(runtime, [this, &callbacks, params](AsyncSslSocket socket, CoSchedulerHandle handle) -> Coroutine<nil> {
            return handleConnection(handle, callbacks, params, std::move(socket));
        });
    }
    
    void Http2Server::run(Runtime& runtime,
                         Http2Router& http2Router,
                         Http2Settings http2Params)
    {
        HTTP2_LOG_INFO("[Http2Server] Starting HTTP/2 server with Http2Router (h2 only)");
        
        // 配置 ALPN 为 h2 only
        configureAlpn(false);
        
        m_server.run(runtime, [this, &http2Router, http2Params](AsyncSslSocket socket, CoSchedulerHandle handle) -> Coroutine<nil> {
            return handleConnectionWithRouter(handle, http2Router, http2Params, std::move(socket));
        });
    }
    
    void Http2Server::run(Runtime& runtime,
                         Http2Router& http2Router,
                         HttpsRouter& http1Router,
                         Http2Settings http2Params,
                         HttpSettings http1Params)
    {
        HTTP2_LOG_INFO("[Http2Server] Starting HTTP/2 server with Http2Router + HttpsRouter (with fallback)");
        
        // 配置 ALPN 为 h2 + http/1.1
        configureAlpn(true);
        
        m_server.run(runtime, [this, &http2Router, &http1Router, http2Params, http1Params](AsyncSslSocket socket, CoSchedulerHandle handle) -> Coroutine<nil> {
            return handleConnectionWithBothRouters(handle, http2Router, http1Router, http2Params, http1Params, std::move(socket));
        });
    }
    
    void Http2Server::run(Runtime& runtime,
                         const Http2Callbacks& http2Callbacks,
                         HttpsRouter& http1Router,
                         Http2Settings http2Params,
                         HttpSettings http1Params)
    {
        // 配置 ALPN 为 h2 + http/1.1
        configureAlpn(true);
        
        m_server.run(runtime, [this, &http2Callbacks, &http1Router, http2Params, http1Params](AsyncSslSocket socket, CoSchedulerHandle handle) -> Coroutine<nil> {
            return handleConnectionWithRouter(handle, http2Callbacks, http1Router, http2Params, http1Params, std::move(socket));
        });
    }
    
    void Http2Server::run(Runtime& runtime,
                         const Http2Callbacks& http2Callbacks,
                         const Http1FallbackFunc& http1Fallback,
                         Http2Settings http2Params)
    {
        HTTP2_LOG_INFO("[Http2Server] Starting HTTP/2 server with HTTP/1.1 fallback (using custom handler)");
        
        // 配置 ALPN 为 h2 + http/1.1
        configureAlpn(true);
        
        m_server.run(runtime, [this, &http2Callbacks, &http1Fallback, http2Params](AsyncSslSocket socket, CoSchedulerHandle handle) -> Coroutine<nil> {
            return handleConnectionWithFallback(handle, http2Callbacks, http1Fallback, http2Params, std::move(socket));
        });
    }
    
    void Http2Server::run(Runtime& runtime, const Http2ConnFunc& handler)
    {
        HTTP2_LOG_INFO("[Http2Server] Starting HTTP/2 server with custom handler");
        
        // 确保 ALPN 已配置
        configureAlpn();
        
        m_server.run(runtime, [handler](AsyncSslSocket socket, CoSchedulerHandle handle) -> Coroutine<nil> {
            HttpsConnection httpsConn(std::move(socket), handle);
            Http2Connection http2Conn = Http2Connection::from(httpsConn);
            return handler(std::move(http2Conn), handle);
        });
    }
    
    void Http2Server::stop()
    {
        HTTP2_LOG_INFO("[Http2Server] Stopping server");
        m_server.stop();
    }
    
    void Http2Server::wait()
    {
        m_server.wait();
    }
    
    Coroutine<nil> Http2Server::handleConnection(CoSchedulerHandle handle,
                                                 const Http2Callbacks& callbacks,
                                                 const Http2Settings& params,
                                                 AsyncSslSocket socket)
    {
        HttpsConnection httpsConn(std::move(socket), handle);
        
        HTTP2_LOG_DEBUG("[Http2Server] New connection accepted");
        
        // 检查 ALPN 协商结果
        std::string alpn_proto = httpsConn.getAlpnProtocol();
        if (alpn_proto.empty() || alpn_proto != "h2") {
            HTTP2_LOG_ERROR("[Http2Server] ALPN negotiation failed or not h2: {}", 
                           alpn_proto.empty() ? "none" : alpn_proto);
            co_await httpsConn.close();
            co_return nil();
        }
        
        HTTP2_LOG_INFO("[Http2Server] ALPN negotiated: h2");
        
        // 创建 HTTP/2 连接
        Http2Connection http2Conn = Http2Connection::from(httpsConn, params);
        
        // 启动 HTTP/2 帧处理循环
        AsyncWaiter<void, Http2Error> waiter;
        auto co = processHttp2Frames(http2Conn, callbacks, params);
        co.then([&waiter](){
            waiter.notify({});
        });
        waiter.appendTask(std::move(co));
        co_await waiter.wait();
        
        HTTP2_LOG_INFO("[Http2Server] Connection closed");
        co_return nil();
    }
    
    Coroutine<nil> Http2Server::handleConnectionWithRouter(CoSchedulerHandle handle,
                                                          Http2Router& http2Router,
                                                          const Http2Settings& http2Params,
                                                          AsyncSslSocket socket)
    {
        HttpsConnection httpsConn(std::move(socket), handle);
        
        HTTP2_LOG_DEBUG("[Http2Server] New connection accepted (with Http2Router)");
        
        // 检查 ALPN 协商结果
        std::string alpn_proto = httpsConn.getAlpnProtocol();
        if (alpn_proto.empty() || alpn_proto != "h2") {
            HTTP2_LOG_ERROR("[Http2Server] ALPN negotiation failed or not h2: {}", 
                           alpn_proto.empty() ? "none" : alpn_proto);
            co_await httpsConn.close();
            co_return nil();
        }
        
        HTTP2_LOG_INFO("[Http2Server] ALPN negotiated: h2");
        
        // 创建 HTTP/2 连接
        Http2Connection http2Conn = Http2Connection::from(httpsConn, http2Params);
        
        // 创建自动路由回调
        Http2Callbacks callbacks;
        callbacks.on_headers = [&http2Router](Http2Connection& conn,
                                             uint32_t stream_id,
                                             const std::map<std::string, std::string>& headers,
                                             bool end_stream) -> Coroutine<nil> {
            std::string method, path;
            for (const auto& [key, value] : headers) {
                if (key == ":method") method = value;
                else if (key == ":path") path = value;
            }
            
            // 使用路由器处理
            bool handled = false;
            http2Router.route(conn, stream_id, method, path, handled).result();
            
            if (!handled) {
                // 未匹配任何路由，返回 404
                HpackEncoder encoder;
                std::string error_body = "404 Not Found";
                std::vector<HpackHeaderField> error_headers = {
                    {":status", "404"},
                    {"content-type", "text/plain"},
                    {"content-length", std::to_string(error_body.length())}
                };
                std::string encoded = encoder.encodeHeaders(error_headers);
                
                auto writer = conn.getWriter({});
                co_await writer.sendHeaders(stream_id, encoded, false, true);
                co_await writer.sendData(stream_id, error_body, true);
                conn.streamManager().removeStream(stream_id);
            }
            
            co_return nil();
        };
        
        callbacks.on_error = [](Http2Connection& conn, const Http2Error& error) -> Coroutine<nil> {
            HTTP2_LOG_ERROR("[Http2Server] Error: {}", error.message());
            co_return nil();
        };
        
        // 启动 HTTP/2 帧处理循环
        AsyncWaiter<void, Http2Error> waiter;
        auto co = processHttp2Frames(http2Conn, callbacks, http2Params);
        co.then([&waiter](){
            waiter.notify({});
        });
        waiter.appendTask(std::move(co));
        co_await waiter.wait();
        
        HTTP2_LOG_INFO("[Http2Server] Connection closed");
        co_return nil();
    }
    
    Coroutine<nil> Http2Server::handleConnectionWithBothRouters(CoSchedulerHandle handle,
                                                               Http2Router& http2Router,
                                                               HttpsRouter& http1Router,
                                                               const Http2Settings& http2Params,
                                                               const HttpSettings& http1Params,
                                                               AsyncSslSocket socket)
    {
        HttpsConnection httpsConn(std::move(socket), handle);
        
        HTTP2_LOG_DEBUG("[Http2Server] New connection accepted (Http2Router + HttpsRouter)");
        
        // 检查 ALPN 协商结果
        std::string alpn_proto = httpsConn.getAlpnProtocol();
        HTTP2_LOG_INFO("[Http2Server] ALPN negotiated: {}", alpn_proto.empty() ? "none" : alpn_proto);
        
        if (alpn_proto == "h2") {
            // HTTP/2 路径
            HTTP2_LOG_INFO("[Http2Server] Using HTTP/2");
            
            // 创建 HTTP/2 连接
            Http2Connection http2Conn = Http2Connection::from(httpsConn, http2Params);
            
            // 创建自动路由回调
            Http2Callbacks callbacks;
            callbacks.on_headers = [&http2Router](Http2Connection& conn,
                                                 uint32_t stream_id,
                                                 const std::map<std::string, std::string>& headers,
                                                 bool end_stream) -> Coroutine<nil> {
                std::string method, path;
                for (const auto& [key, value] : headers) {
                    if (key == ":method") method = value;
                    else if (key == ":path") path = value;
                }
                
                // 使用路由器处理
                bool handled = false;
                http2Router.route(conn, stream_id, method, path, handled).result();
                
                if (!handled) {
                    // 未匹配任何路由，返回 404
                    HpackEncoder encoder;
                    std::string error_body = "404 Not Found";
                    std::vector<HpackHeaderField> error_headers = {
                        {":status", "404"},
                        {"content-type", "text/plain"},
                        {"content-length", std::to_string(error_body.length())}
                    };
                    std::string encoded = encoder.encodeHeaders(error_headers);
                    
                    auto writer = conn.getWriter({});
                    co_await writer.sendHeaders(stream_id, encoded, false, true);
                    co_await writer.sendData(stream_id, error_body, true);
                    conn.streamManager().removeStream(stream_id);
                }
                
                co_return nil();
            };
            
            callbacks.on_error = [](Http2Connection& conn, const Http2Error& error) -> Coroutine<nil> {
                HTTP2_LOG_ERROR("[Http2Server] Error: {}", error.message());
                co_return nil();
            };
            
            // 启动 HTTP/2 帧处理循环
            AsyncWaiter<void, Http2Error> waiter;
            auto co = processHttp2Frames(http2Conn, callbacks, http2Params);
            co.then([&waiter](){ waiter.notify({}); });
            waiter.appendTask(std::move(co));
            co_await waiter.wait();
        } else {
            // HTTP/1.1 降级路径
            HTTP2_LOG_INFO("[Http2Server] Fallback to HTTP/1.1");
            AsyncWaiter<void, Http2Error> waiter;
            auto co = handleHttp1Connection(handle, http1Router, http1Params, httpsConn);
            co.then([&waiter](){ waiter.notify({}); });
            waiter.appendTask(std::move(co));
            co_await waiter.wait();
        }
        
        HTTP2_LOG_INFO("[Http2Server] Connection closed");
        co_return nil();
    }
    
    Coroutine<nil> Http2Server::handleConnectionWithRouter(CoSchedulerHandle handle,
                                                          const Http2Callbacks& http2Callbacks,
                                                          HttpsRouter& http1Router,
                                                          const Http2Settings& http2Params,
                                                          const HttpSettings& http1Params,
                                                          AsyncSslSocket socket)
    {
        HttpsConnection httpsConn(std::move(socket), handle);
        
        HTTP2_LOG_DEBUG("[Http2Server] New connection accepted");
        
        // 检查 ALPN 协商结果
        std::string alpn_proto = httpsConn.getAlpnProtocol();
        HTTP2_LOG_INFO("[Http2Server] ALPN negotiated: {}", alpn_proto.empty() ? "none" : alpn_proto);
        
        if (alpn_proto == "h2") {
            // HTTP/2 路径
            HTTP2_LOG_INFO("[Http2Server] Using HTTP/2");
            
            // 创建 HTTP/2 连接
            Http2Connection http2Conn = Http2Connection::from(httpsConn, http2Params);
            
            // 启动 HTTP/2 帧处理循环
            AsyncWaiter<void, Http2Error> waiter;
            auto co = processHttp2Frames(http2Conn, http2Callbacks, http2Params);
            co.then([&waiter](){ waiter.notify({}); });
            waiter.appendTask(std::move(co));
            co_await waiter.wait();
        } else {
            AsyncWaiter<void, Http2Error> waiter;
            auto co = handleHttp1Connection(handle, http1Router, http1Params, httpsConn);
            co.then([&waiter](){ waiter.notify({}); });
            waiter.appendTask(std::move(co));
            co_await waiter.wait();
        }
        
        HTTP2_LOG_INFO("[Http2Server] Connection closed");
        co_return nil();
    }
    
    Coroutine<nil> Http2Server::handleConnectionWithFallback(CoSchedulerHandle handle,
                                                            const Http2Callbacks& http2Callbacks,
                                                            const Http1FallbackFunc& http1Fallback,
                                                            const Http2Settings& http2Params,
                                                            AsyncSslSocket socket)
    {
        HttpsConnection httpsConn(std::move(socket), handle);
        
        HTTP2_LOG_DEBUG("[Http2Server] New connection accepted");
        
        // 检查 ALPN 协商结果
        std::string alpn_proto = httpsConn.getAlpnProtocol();
        HTTP2_LOG_INFO("[Http2Server] ALPN negotiated: {}", alpn_proto.empty() ? "none" : alpn_proto);
        
        if (alpn_proto == "h2") {
            // HTTP/2 路径
            HTTP2_LOG_INFO("[Http2Server] Using HTTP/2");
            
            // 创建 HTTP/2 连接
            Http2Connection http2Conn = Http2Connection::from(httpsConn, http2Params);
            
            // 启动 HTTP/2 帧处理循环
            AsyncWaiter<void, Http2Error> waiter;
            auto co = processHttp2Frames(http2Conn, http2Callbacks, http2Params);
            co.then([&waiter](){ waiter.notify({}); });
            waiter.appendTask(std::move(co));
            co_await waiter.wait();
        } else {
            // HTTP/1.1 降级路径 - 使用自定义处理器
            HTTP2_LOG_INFO("[Http2Server] Fallback to HTTP/1.1 (custom handler)");
            AsyncWaiter<void, Http2Error> waiter;
            auto co = http1Fallback(std::move(httpsConn), handle);
            co.then([&waiter](){ waiter.notify({}); });
            waiter.appendTask(std::move(co));
            co_await waiter.wait();
        }
        
        HTTP2_LOG_INFO("[Http2Server] Connection closed");
        co_return nil();
    }
    
    Coroutine<nil> Http2Server::handleHttp1Connection(CoSchedulerHandle handle,
                                                      HttpsRouter& router,
                                                      const HttpSettings& params,
                                                      HttpsConnection& conn)
    {
        HTTP2_LOG_DEBUG("[Http2Server] Processing HTTP/1.1 connection");
        
        while(true) 
        {
            // 检查连接状态
            if (conn.isClosed()) {
                HTTP2_LOG_DEBUG("[Http2Server] HTTP/1.1 connection already closed");
                co_return nil();
            }
            
            auto reader = conn.getRequestReader(params);
            auto writer = conn.getResponseWriter(params);
            
            HTTP2_LOG_DEBUG("[Http2Server] Waiting for HTTP/1.1 request...");
            auto request_res = co_await reader.getRequest();
            
            if(!request_res) {
                if(request_res.error().code() == HttpErrorCode::kHttpError_ConnectionClose) {
                    HTTP2_LOG_DEBUG("[Http2Server] HTTP/1.1 connection closed by peer");
                    co_await conn.close();
                    co_return nil();
                }
                HTTP2_LOG_ERROR("[Http2Server] HTTP/1.1 request error: {}", request_res.error().message());
                auto response = HttpUtils::defaultHttpResponse(request_res.error().toHttpStatusCode());
                response.header().headerPairs().addHeaderPair("Connection", "close");
                auto response_res = co_await writer.reply(response);
                if(!response_res) {
                    HTTP2_LOG_ERROR("[Http2Server] HTTP/1.1 reply error: {}", response_res.error().message());
                } 
                co_await conn.close();
                co_return nil();
            }
            
            auto& request = request_res.value();
            HTTP2_LOG_INFO("[Http2Server] HTTP/1.1 request: {} {}", 
                          httpMethodToString(request.header().method()), 
                          request.header().uri());
            
            auto route_res = co_await router.route(request, conn);
            
            if(!route_res) {
                HTTP2_LOG_DEBUG("[Http2Server] HTTP/1.1 route error: {}", route_res.error().message());
                auto response = HttpUtils::defaultHttpResponse(route_res.error().toHttpStatusCode());
                auto response_res = co_await writer.reply(response);
                if(!response_res) {
                    co_await conn.close();
                    HTTP2_LOG_ERROR("[Http2Server] HTTP/1.1 reply error: {}", response_res.error().message());
                    co_return nil();
                }
                continue;
            }
            
            if(request.header().isConnectionClose()) {
                if(!conn.isClosed()) {
                    co_await conn.close();
                }
            } 
            if (conn.isClosed()) {
                HTTP2_LOG_DEBUG("[Http2Server] HTTP/1.1 connection closed");
                co_return nil();
            }
        }
    }
    
    Coroutine<nil> Http2Server::processHttp2Frames(Http2Connection& connection,
                                                   const Http2Callbacks& callbacks,
                                                   const Http2Settings& params)
    {
        HTTP2_LOG_DEBUG("[Http2Server] Starting HTTP/2 frame processing loop");
        
        auto reader = connection.getReader(params);
        auto writer = connection.getWriter(params);
        
        // 1. 服务器先发送 SETTINGS（HTTP/2 over TLS 要求）
        HTTP2_LOG_DEBUG("[Http2Server] Sending server SETTINGS...");
        auto send_settings_res = co_await writer.sendSettings(params);
        if (!send_settings_res) {
            HTTP2_LOG_ERROR("[Http2Server] Failed to send SETTINGS: {}", send_settings_res.error().message());
            if (callbacks.on_error) {
                callbacks.on_error(connection, send_settings_res.error());
            }
            co_return nil();
        }
        HTTP2_LOG_INFO("[Http2Server] Server SETTINGS sent");
        
        // 2. 读取客户端 preface
        HTTP2_LOG_DEBUG("[Http2Server] Waiting for client preface...");
        auto preface_res = co_await reader.readPreface();
        if (!preface_res) {
            HTTP2_LOG_ERROR("[Http2Server] Failed to read client preface: {}", preface_res.error().message());
            if (callbacks.on_error) {
                callbacks.on_error(connection, preface_res.error());
            }
            co_return nil();
        }
        HTTP2_LOG_INFO("[Http2Server] Client preface received");
        
        // 3. 进入帧处理循环
        HTTP2_LOG_DEBUG("[Http2Server] Entering frame processing loop");
        HpackDecoder hpack_decoder;
        int frame_count = 0;
        
        while (true) {
            // 检查连接是否已关闭
            if (connection.isClosed()) {
                HTTP2_LOG_INFO("[Http2Server] Connection closed, exiting frame loop");
                break;
            }
            
            // 接收帧
            auto frame_res = co_await reader.readFrame();
            
            if (!frame_res) {
                HTTP2_LOG_ERROR("[Http2Server] Failed to read frame: {}", frame_res.error().message());
                if (callbacks.on_error) {
                    callbacks.on_error(connection, frame_res.error());
                }
                break;
            }
            
            auto frame = frame_res.value();
            frame_count++;
            
            HTTP2_LOG_INFO("[Http2Server] Frame #{}: type={}, stream={}, length={}, flags=0x{:02X}",
                          frame_count,
                          http2FrameTypeToString(frame->type()),
                          frame->streamId(),
                          frame->length(),
                          frame->flags());
            
            bool should_continue = true;
            
            // 根据帧类型分发到对应的回调
            switch (frame->type()) {
                case Http2FrameType::HEADERS: {
                    auto headers_frame = std::dynamic_pointer_cast<Http2HeadersFrame>(frame);
                    if (headers_frame && callbacks.on_headers) {
                        auto headers_vec_res = headers_frame->decodeHeaders(hpack_decoder);
                        if (headers_vec_res) {
                            // 转换为 map
                            std::map<std::string, std::string> headers_map;
                            for (const auto& field : headers_vec_res.value()) {
                                headers_map[field.name] = field.value;
                            }
                            
                            uint32_t stream_id = headers_frame->streamId();
                            bool end_stream = headers_frame->endStream();
                            
                            // 创建 stream（如果不存在）
                            auto& stream_manager = connection.streamManager();
                            if (!stream_manager.getStream(stream_id)) {
                                auto create_result = stream_manager.createStream(stream_id);
                                if (!create_result) {
                                    HTTP2_LOG_ERROR("[Http2Server] Failed to create stream {}: {}",
                                                   stream_id, create_result.error().message());
                                    if (callbacks.on_error) {
                                        callbacks.on_error(connection, create_result.error());
                                    }
                                    break;
                                }
                            }
                            
                            // 调用回调
                            AsyncWaiter<void, Http2Error> callback_waiter;
                            auto callback_coro = callbacks.on_headers(connection, stream_id, headers_map, end_stream);
                            callback_coro.then([&callback_waiter](){ callback_waiter.notify({}); });
                            callback_waiter.appendTask(std::move(callback_coro));
                            co_await callback_waiter.wait();
                        } else {
                            HTTP2_LOG_ERROR("[Http2Server] Failed to decode headers: {}", headers_vec_res.error().message());
                            if (callbacks.on_error) {
                                callbacks.on_error(connection, headers_vec_res.error());
                            }
                            should_continue = false;
                        }
                    }
                    break;
                }
                
                case Http2FrameType::DATA: {
                    auto data_frame = std::dynamic_pointer_cast<Http2DataFrame>(frame);
                    if (data_frame && callbacks.on_data) {
                        bool end_stream = data_frame->endStream();
                        HTTP2_LOG_INFO("[Http2Server] DATA frame on stream {}, size={}, end_stream={}",
                                      data_frame->streamId(), data_frame->data().size(), end_stream);
                        
                        AsyncWaiter<void, Http2Error> callback_waiter;
                        auto callback_coro = callbacks.on_data(connection, data_frame->streamId(),
                                                              data_frame->data(), end_stream);
                        callback_coro.then([&callback_waiter](){ callback_waiter.notify({}); });
                        callback_waiter.appendTask(std::move(callback_coro));
                        co_await callback_waiter.wait();
                    }
                    break;
                }
                
                case Http2FrameType::SETTINGS: {
                    auto settings_frame = std::dynamic_pointer_cast<Http2SettingsFrame>(frame);
                    if (settings_frame) {
                        bool is_ack = settings_frame->isAck();
                        HTTP2_LOG_INFO("[Http2Server] SETTINGS frame, ack={}", is_ack);
                        
                        if (callbacks.on_settings) {
                            AsyncWaiter<void, Http2Error> callback_waiter;
                            auto callback_coro = callbacks.on_settings(connection, settings_frame->settings(), is_ack);
                            callback_coro.then([&callback_waiter](){ callback_waiter.notify({}); });
                            callback_waiter.appendTask(std::move(callback_coro));
                            co_await callback_waiter.wait();
                        }
                        
                        // 如果不是 ACK，自动回复 SETTINGS ACK
                        if (!is_ack) {
                            HTTP2_LOG_DEBUG("[Http2Server] Sending SETTINGS ACK");
                            auto ack_res = co_await writer.sendSettingsAck();
                            if (!ack_res) {
                                HTTP2_LOG_ERROR("[Http2Server] Failed to send SETTINGS ACK: {}", ack_res.error().message());
                                if (callbacks.on_error) {
                                    callbacks.on_error(connection, ack_res.error());
                                }
                                should_continue = false;
                            }
                        }
                    }
                    break;
                }
                
                case Http2FrameType::PING: {
                    auto ping_frame = std::dynamic_pointer_cast<Http2PingFrame>(frame);
                    if (ping_frame) {
                        bool is_ack = ping_frame->isAck();
                        uint64_t ping_data = ping_frame->data();
                        HTTP2_LOG_INFO("[Http2Server] PING frame, ack={}, data={}", is_ack, ping_data);
                        
                        if (callbacks.on_ping) {
                            AsyncWaiter<void, Http2Error> callback_waiter;
                            auto callback_coro = callbacks.on_ping(connection, ping_data, is_ack);
                            callback_coro.then([&callback_waiter](){ callback_waiter.notify({}); });
                            callback_waiter.appendTask(std::move(callback_coro));
                            co_await callback_waiter.wait();
                        }
                        
                        // 如果不是 ACK，自动回复 PING ACK
                        if (!is_ack) {
                            HTTP2_LOG_DEBUG("[Http2Server] Sending PING ACK");
                            auto pong_res = co_await writer.sendPing(ping_data, true);
                            if (!pong_res) {
                                HTTP2_LOG_ERROR("[Http2Server] Failed to send PING ACK: {}", pong_res.error().message());
                                if (callbacks.on_error) {
                                    callbacks.on_error(connection, pong_res.error());
                                }
                                should_continue = false;
                            }
                        }
                    }
                    break;
                }
                
                case Http2FrameType::GOAWAY: {
                    auto goaway_frame = std::dynamic_pointer_cast<Http2GoAwayFrame>(frame);
                    if (goaway_frame) {
                        HTTP2_LOG_INFO("[Http2Server] GOAWAY frame, last_stream_id={}, error_code={}",
                                      goaway_frame->lastStreamId(),
                                      http2ErrorCodeToString(goaway_frame->errorCode()));
                        
                        if (callbacks.on_goaway) {
                            AsyncWaiter<void, Http2Error> callback_waiter;
                            auto callback_coro = callbacks.on_goaway(connection, goaway_frame->lastStreamId(),
                                                                    goaway_frame->errorCode(), goaway_frame->debugData());
                            callback_coro.then([&callback_waiter](){ callback_waiter.notify({}); });
                            callback_waiter.appendTask(std::move(callback_coro));
                            co_await callback_waiter.wait();
                        }
                        should_continue = false;
                    }
                    break;
                }
                
                case Http2FrameType::WINDOW_UPDATE: {
                    auto window_frame = std::dynamic_pointer_cast<Http2WindowUpdateFrame>(frame);
                    if (window_frame && callbacks.on_window_update) {
                        HTTP2_LOG_DEBUG("[Http2Server] WINDOW_UPDATE frame, stream_id={}, increment={}",
                                       window_frame->streamId(), window_frame->windowSizeIncrement());
                        
                        AsyncWaiter<void, Http2Error> callback_waiter;
                        auto callback_coro = callbacks.on_window_update(connection, window_frame->streamId(),
                                                                       window_frame->windowSizeIncrement());
                        callback_coro.then([&callback_waiter](){ callback_waiter.notify({}); });
                        callback_waiter.appendTask(std::move(callback_coro));
                        co_await callback_waiter.wait();
                    }
                    break;
                }
                
                case Http2FrameType::RST_STREAM: {
                    auto rst_frame = std::dynamic_pointer_cast<Http2RstStreamFrame>(frame);
                    if (rst_frame && callbacks.on_rst_stream) {
                        HTTP2_LOG_INFO("[Http2Server] RST_STREAM frame, stream_id={}, error_code={}",
                                      rst_frame->streamId(), http2ErrorCodeToString(rst_frame->errorCode()));
                        
                        AsyncWaiter<void, Http2Error> callback_waiter;
                        auto callback_coro = callbacks.on_rst_stream(connection, rst_frame->streamId(),
                                                                     rst_frame->errorCode());
                        callback_coro.then([&callback_waiter](){ callback_waiter.notify({}); });
                        callback_waiter.appendTask(std::move(callback_coro));
                        co_await callback_waiter.wait();
                    }
                    break;
                }
                
                case Http2FrameType::PRIORITY: {
                    auto priority_frame = std::dynamic_pointer_cast<Http2PriorityFrame>(frame);
                    if (priority_frame && callbacks.on_priority) {
                        HTTP2_LOG_DEBUG("[Http2Server] PRIORITY frame, stream_id={}", priority_frame->streamId());
                        
                        AsyncWaiter<void, Http2Error> callback_waiter;
                        auto callback_coro = callbacks.on_priority(connection, priority_frame->streamId(),
                                                                   priority_frame->streamDependency(),
                                                                   priority_frame->weight(),
                                                                   priority_frame->exclusive());
                        callback_coro.then([&callback_waiter](){ callback_waiter.notify({}); });
                        callback_waiter.appendTask(std::move(callback_coro));
                        co_await callback_waiter.wait();
                    }
                    break;
                }
                
                default:
                    HTTP2_LOG_WARN("[Http2Server] Unhandled frame type: {}", http2FrameTypeToString(frame->type()));
                    break;
            }
            
            if (!should_continue) {
                HTTP2_LOG_INFO("[Http2Server] Stopping frame processing loop");
                break;
            }
        }
        
        HTTP2_LOG_INFO("[Http2Server] Frame processing complete, processed {} frames", frame_count);
        co_return nil();
    }
    
    // Http2ServerBuilder Implementation
    
    Http2ServerBuilder::Http2ServerBuilder(const std::string& cert_file, const std::string& key_file)
        : m_cert(cert_file), m_key(key_file)
    {
    }
    
    Http2ServerBuilder& Http2ServerBuilder::addListen(const Host& host)
    {
        m_host = host;
        return *this;
    }
    
    Http2ServerBuilder& Http2ServerBuilder::threads(int threads)
    {
        m_threads = threads;
        return *this;
    }
    
    Http2Server Http2ServerBuilder::build()
    {
        HTTP2_LOG_DEBUG("[Http2ServerBuilder] Building HTTP/2 server");
        
        // 创建 TcpSslServer
        TcpSslServerBuilder builder(m_cert, m_key);
        auto server = builder.backlog(DEFAULT_TCP_BACKLOG_SIZE)
                            .addListen(m_host)
                            .build();
        
        HTTP2_LOG_INFO("[Http2ServerBuilder] HTTP/2 server created for {}:{}", m_host.ip, m_host.port);
        
        return Http2Server(std::move(server), m_cert, m_key);
    }
}

