#include "HttpsServer.h"
#include "galay-http/kernel/http/HttpsConnection.h"
#include "galay-http/kernel/http/HttpsReader.h"
#include "galay-http/kernel/http/HttpsWriter.h"
#include "galay-http/kernel/http/HttpParams.hpp"
#include "galay-http/kernel/http2/Http2Connection.h"
#include "galay-http/kernel/http2/Http2Reader.h"
#include "galay-http/kernel/http2/Http2Writer.h"
#include "galay-http/protoc/http2/Http2Error.h"
#include "galay-http/protoc/alpn/AlpnProtocol.h"
#include "galay-http/utils/HttpUtils.h"
#include "galay-http/utils/HttpDebugLog.h"
#include "galay-http/utils/HttpsDebugLog.h"
#include "galay-http/utils/Http2DebugLog.h"
#include "galay/kernel/coroutine/AsyncWaiter.hpp"
#include "galay/kernel/runtime/Runtime.h"
#include "galay/kernel/async/AsyncFactory.h"
#include "galay/common/Common.h"

namespace galay::http
{
    // 辅助方法：确保 ALPN 已配置
    void HttpsServer::ensureALPNConfigured()
    {
        if (m_ssl_configured) {
            return; // 已经配置过了
        }
        
        // 初始化 SSL 上下文
        m_server.initializeSSLContext();
        SSL_CTX* ctx = m_server.getSSLContext();
        
        if (ctx) {
            // 配置 ALPN
            if (m_http2_enabled) {
                AlpnProtocolList alpn_list = AlpnProtocolList::http2WithFallback();
                if (configureServerAlpn(ctx, alpn_list)) {
                    HTTPS_LOG_INFO("[HttpsServer] ALPN configured: h2, http/1.1");
                } else {
                    HTTPS_LOG_WARN("[HttpsServer] Failed to configure ALPN");
                }
            } else {
                AlpnProtocolList alpn_list = AlpnProtocolList::http11Only();
                configureServerAlpn(ctx, alpn_list);
                HTTPS_LOG_INFO("[HttpsServer] ALPN configured: http/1.1 only");
            }
            m_ssl_configured = true;
        } else {
            HTTPS_LOG_WARN("[HttpsServer] Cannot get SSL_CTX, ALPN not configured");
        }
    }

    void HttpsServer::listen(const Host &host)
    {
        HTTPS_LOG_DEBUG("[HttpsServer] listen() called for {}:{}", host.ip, host.port);
        m_server.listenOn(host, DEFAULT_TCP_BACKLOG_SIZE);
        HTTPS_LOG_INFO("[HttpsServer] Listening on {}:{}", host.ip, host.port);
    }

    void HttpsServer::run(Runtime& runtime, const HttpsConnFunc& handler)
    {
        HTTPS_LOG_DEBUG("[HttpsServer] run() with custom handler");
        m_server.run(runtime, [handler, &runtime](AsyncSslSocket socket) -> Coroutine<nil> { 
            HTTPS_LOG_DEBUG("[HttpsServer] New SSL connection accepted");
            AsyncFactory factory = runtime.getAsyncFactory();
            HttpsConnection conn(std::move(socket), factory.getTimerGenerator());
            return handler(std::move(conn));
        });
    }

    void HttpsServer::run(Runtime& runtime, HttpsRouter& router, HttpSettings params)
    {
        HTTPS_LOG_DEBUG("[HttpsServer] run() with router (HTTP/1.1 only)");
        HTTPS_LOG_DEBUG("[HttpsServer] HTTP/2 enabled: {}", m_http2_enabled);
        
        // 确保 ALPN 已配置
        ensureALPNConfigured();
        
        m_server.run(runtime, [this, &runtime, &router, params](AsyncSslSocket socket) -> Coroutine<nil> {
            HTTPS_LOG_DEBUG("[HttpsServer] New SSL connection accepted (router mode)");
            return handleConnection(runtime, router, params, std::move(socket));
        });
    }
    
    void HttpsServer::run(Runtime& runtime, 
                         HttpsRouter& http1Router, 
                         const Http2ConnFunc& http2Handler,
                         HttpSettings httpParams,
                         Http2Settings http2Params)
    {
        // 确保 ALPN 已配置
        ensureALPNConfigured();
        
        m_server.run(runtime, [this, &runtime, &http1Router, &http2Handler, httpParams, http2Params](AsyncSslSocket socket) -> Coroutine<nil> {
            return handleConnectionWithHttp2(runtime, http1Router, http2Handler, httpParams, http2Params, std::move(socket));
        });
    }
    
    void HttpsServer::run(Runtime& runtime,
                         HttpsRouter& http1Router,
                         const Http2Callbacks& http2Callbacks,
                         HttpSettings httpParams,
                         Http2Settings http2Params)
    {
        m_server.run(runtime, [this, &runtime, &http1Router, &http2Callbacks, httpParams, http2Params](AsyncSslSocket socket) -> Coroutine<nil> {
            return handleConnectionWithHttp2Callbacks(runtime, http1Router, http2Callbacks, httpParams, http2Params, std::move(socket));
        });
    }

    void HttpsServer::stop()
    {
        m_server.stop();
    }

    void HttpsServer::wait()
    {
        m_server.wait();
    }

    Coroutine<nil> HttpsServer::handleConnection(Runtime& runtime, HttpsRouter& router, HttpSettings params, AsyncSslSocket socket)
    {
        AsyncFactory factory = runtime.getAsyncFactory();
        HttpsConnection conn(std::move(socket), factory.getTimerGenerator());
        
        HTTPS_LOG_DEBUG("[HttpsServer] handleConnection() started");
        // 检查 ALPN 协商结果
        std::string alpn_proto = conn.getAlpnProtocol();
        HTTPS_LOG_DEBUG("[HttpsServer] ALPN negotiated: {}", alpn_proto.empty() ? "none" : alpn_proto);
        
        // 如果协商了 h2，但此方法不支持 HTTP/2，拒绝连接
        if (!alpn_proto.empty() && alpn_proto == "h2") {
            HTTPS_LOG_WARN("[HttpsServer] ALPN negotiated h2, but run(router) method only supports HTTP/1.1");
            HTTPS_LOG_WARN("[HttpsServer] Please use run(router, http2Callbacks) to handle HTTP/2 connections");
            HTTPS_LOG_WARN("[HttpsServer] Or disable HTTP/2 in HttpsServerBuilder");
            // 主动关闭连接
            co_await conn.close();
            co_return nil();
        }
        
        HTTPS_LOG_INFO("[HttpsServer] Using HTTP/1.1 protocol");
        
        while(true) 
        {
            // 检查连接状态
            if (conn.isClosed()) {
                HTTPS_LOG_DEBUG("[HttpsServer] Connection already closed");
                co_return nil();
            }
            
            auto reader = conn.getRequestReader(params);
            auto writer = conn.getResponseWriter(params);
            
            HTTPS_LOG_DEBUG("[HttpsServer] About to call reader.getRequest()");
            auto request_res = co_await reader.getRequest();
            HTTPS_LOG_DEBUG("[HttpsServer] reader.getRequest() completed");
            
            if(!request_res) {
                if(request_res.error().code() == HttpErrorCode::kHttpError_ConnectionClose) {
                    HTTPS_LOG_DEBUG("[HttpsServer] Connection closed by peer");
                    co_await conn.close();
                    co_return nil();
                }
                HTTPS_LOG_ERROR("[HttpsServer] Request error: {}", request_res.error().message());
                auto response = HttpUtils::defaultHttpResponse(request_res.error().toHttpStatusCode());
                response.header().headerPairs().addHeaderPair("Connection", "close");
                auto response_res = co_await writer.reply(response);
                if(!response_res) {
                    HTTPS_LOG_ERROR("[HttpsServer] Reply error: {}", response_res.error().message());
                } 
                co_await conn.close();
                co_return nil();
            }
            auto& request = request_res.value();
            SERVER_REQUEST_LOG(request.header().method(), request.header().uri());
            
            auto route_res = co_await router.route(request, conn);
            
            if(!route_res) {
                HTTPS_LOG_DEBUG("[HttpsServer] Route error: {}", route_res.error().message());
                auto response = HttpUtils::defaultHttpResponse(route_res.error().toHttpStatusCode());
                auto response_res = co_await writer.reply(response);
                if(!response_res) {
                    co_await conn.close();
                    HTTPS_LOG_ERROR("[HttpsServer] Reply error: {}", response_res.error().message());
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
                HTTPS_LOG_DEBUG("[HttpsServer] Connection closed");
                co_return nil();
            }
        }
    }
    
    Coroutine<nil> HttpsServer::handleConnectionWithHttp2(Runtime& runtime, 
                                                          HttpsRouter& http1Router,
                                                          const Http2ConnFunc& http2Handler,
                                                          HttpSettings httpParams,
                                                          Http2Settings http2Params,
                                                          AsyncSslSocket socket)
    {
        AsyncFactory factory = runtime.getAsyncFactory();
        HttpsConnection conn(std::move(socket), factory.getTimerGenerator());
        
        HTTP_LOG_DEBUG("[HttpsServer] New HTTPS connection");
        
        // 检测 ALPN 协议
        if (m_http2_enabled && conn.isHttp2()) {
            // HTTP/2 路径
            HTTP_LOG_INFO("[HttpsServer] ALPN negotiated: h2 - Using HTTP/2");
            
            // 创建 HTTP/2 连接
            Http2Connection http2Conn = Http2Connection::from(reinterpret_cast<HttpConnection&>(conn));
            
            // 调用 HTTP/2 处理器（不需要 co_await，因为它返回 Coroutine<nil>）
            runtime.schedule(http2Handler(std::move(http2Conn)));
        } else {
            // HTTP/1.1 路径
            std::string protocol = conn.getAlpnProtocol();
            if (!protocol.empty()) {
                HTTP_LOG_INFO("[HttpsServer] ALPN negotiated: {} - Using HTTP/1.1", protocol);
            } else {
                HTTP_LOG_INFO("[HttpsServer] No ALPN - Using HTTP/1.1");
            }
            
            // 使用标准的 HTTP/1.1 处理
            while(true) 
            {
                if (conn.isClosed()) {
                    HTTP_LOG_DEBUG("[HttpsServer] Connection already closed");
                    co_return nil();
                }
                
                auto reader = conn.getRequestReader(httpParams);
                auto writer = conn.getResponseWriter(httpParams);
                
                auto request_res = co_await reader.getRequest();
                
                if(!request_res) {
                    if(request_res.error().code() == HttpErrorCode::kHttpError_ConnectionClose) {
                        HTTP_LOG_DEBUG("[HttpsServer] Connection closed by peer");
                        co_await conn.close();
                        co_return nil();
                    }
                    HTTP_LOG_DEBUG("[HttpsServer] Request error: {}", request_res.error().message());
                    auto response = HttpUtils::defaultHttpResponse(request_res.error().toHttpStatusCode());
                    response.header().headerPairs().addHeaderPair("Connection", "close");
                    auto response_res = co_await writer.reply(response);
                    if(!response_res) {
                        HTTP_LOG_ERROR("[HttpsServer] Reply error: {}", response_res.error().message());
                    } 
                    co_await conn.close();
                    co_return nil();
                }
                
                auto& request = request_res.value();
                SERVER_REQUEST_LOG(request.header().method(), request.header().uri());
                
                auto route_res = co_await http1Router.route(request, conn);
                
                if(!route_res) {
                    HTTP_LOG_DEBUG("[HttpsServer] Route error: {}", route_res.error().message());
                    auto response = HttpUtils::defaultHttpResponse(route_res.error().toHttpStatusCode());
                    auto response_res = co_await writer.reply(response);
                    if(!response_res) {
                        co_await conn.close();
                        HTTP_LOG_ERROR("[HttpsServer] Reply error: {}", response_res.error().message());
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
                    HTTP_LOG_DEBUG("[HttpsServer] Connection closed");
                    co_return nil();
                }
            }
        }
        
        co_return nil();
    }
    
    Coroutine<nil> HttpsServer::handleConnectionWithHttp2Callbacks(Runtime& runtime,
                                                                   HttpsRouter& http1Router,
                                                                   const Http2Callbacks& http2Callbacks,
                                                                   HttpSettings httpParams,
                                                                   Http2Settings http2Params,
                                                                   AsyncSslSocket socket)
    {
        AsyncFactory factory = runtime.getAsyncFactory();
        HttpsConnection conn(std::move(socket), factory.getTimerGenerator());
        
        HTTP_LOG_DEBUG("[HttpsServer] New HTTPS connection");
        
        // 检测 ALPN 协议
        if (m_http2_enabled && conn.isHttp2()) {
            // HTTP/2 路径 - 使用回调处理
            HTTP_LOG_INFO("[HttpsServer] ALPN negotiated: h2 - Using HTTP/2 with callbacks");
            
            // 创建 HTTP/2 连接
            Http2Connection http2Conn = Http2Connection::from(conn);
            AsyncWaiter<void, Http2Error> waiter;
            auto co = processHttp2Frames(http2Conn, http2Callbacks, http2Params);
            co.then([&waiter](){
                waiter.notify({});
            });
            waiter.appendTask(std::move(co));
            // 启动帧处理循环
            co_await waiter.wait();
            HTTP_LOG_INFO("[HttpsServer] HTTP/2 frame processing complete");
        } else {
            // HTTP/1.1 路径
            std::string protocol = conn.getAlpnProtocol();
            if (!protocol.empty()) {
                HTTP_LOG_INFO("[HttpsServer] ALPN negotiated: {} - Using HTTP/1.1", protocol);
            } else {
                HTTP_LOG_INFO("[HttpsServer] No ALPN - Using HTTP/1.1");
            }
            
            // 使用标准的 HTTP/1.1 处理
            while(true) 
            {
                if (conn.isClosed()) {
                    HTTP_LOG_DEBUG("[HttpsServer] Connection already closed");
                    co_return nil();
                }
                
                auto reader = conn.getRequestReader(httpParams);
                auto writer = conn.getResponseWriter(httpParams);
                
                auto request_res = co_await reader.getRequest();
                
                if(!request_res) {
                    if(request_res.error().code() == HttpErrorCode::kHttpError_ConnectionClose) {
                        HTTP_LOG_DEBUG("[HttpsServer] Connection closed by peer");
                        co_await conn.close();
                        co_return nil();
                    }
                    HTTP_LOG_DEBUG("[HttpsServer] Request error: {}", request_res.error().message());
                    auto response = HttpUtils::defaultHttpResponse(request_res.error().toHttpStatusCode());
                    response.header().headerPairs().addHeaderPair("Connection", "close");
                    auto response_res = co_await writer.reply(response);
                    if(!response_res) {
                        HTTP_LOG_ERROR("[HttpsServer] Reply error: {}", response_res.error().message());
                    } 
                    co_await conn.close();
                    co_return nil();
                }
                
                auto& request = request_res.value();
                SERVER_REQUEST_LOG(request.header().method(), request.header().uri());
                
                auto route_res = co_await http1Router.route(request, conn);
                
                if(!route_res) {
                    HTTP_LOG_DEBUG("[HttpsServer] Route error: {}", route_res.error().message());
                    auto response = HttpUtils::defaultHttpResponse(route_res.error().toHttpStatusCode());
                    auto response_res = co_await writer.reply(response);
                    if(!response_res) {
                        co_await conn.close();
                        HTTP_LOG_ERROR("[HttpsServer] Reply error: {}", response_res.error().message());
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
                    HTTP_LOG_DEBUG("[HttpsServer] Connection closed");
                    co_return nil();
                }
            }
        }
        
        co_return nil();
    }
    
    Coroutine<nil> HttpsServer::processHttp2Frames(Http2Connection& connection,
                                                   const Http2Callbacks& callbacks,
                                                   const Http2Settings& params)
    {
        HTTP2_LOG_DEBUG("[HttpsServer] Starting HTTP/2 frame processing loop");
        
        auto reader = connection.getReader(params);
        auto writer = connection.getWriter(params);
        
        // 1. 服务器先发送 SETTINGS（HTTP/2 over TLS 要求）
        HTTP2_LOG_DEBUG("[HttpsServer] Sending server SETTINGS...");
        auto send_settings_res = co_await writer.sendSettings(params);
        if (!send_settings_res) {
            HTTP2_LOG_ERROR("[HttpsServer] Failed to send SETTINGS: {}", send_settings_res.error().message());
            if (callbacks.on_error) {
                callbacks.on_error(connection, send_settings_res.error());
            }
            co_return nil();
        }
        HTTP2_LOG_INFO("[HttpsServer] Server SETTINGS sent");
        
        // 2. 读取客户端 preface
        HTTP2_LOG_DEBUG("[HttpsServer] Waiting for client preface...");
        auto preface_res = co_await reader.readPreface();
        if (!preface_res) {
            HTTP2_LOG_ERROR("[HttpsServer] Failed to read client preface: {}", preface_res.error().message());
            if (callbacks.on_error) {
                callbacks.on_error(connection, preface_res.error());
            }
            co_return nil();
        }
        HTTP2_LOG_INFO("[HttpsServer] Client preface received");
        
        // 3. 进入帧处理循环
        HTTP2_LOG_DEBUG("[HttpsServer] Entering frame processing loop");
        HpackDecoder hpack_decoder;  // 创建 HPACK 解码器
        int frame_count = 0;
        while (true) {
            // 检查连接是否已关闭
            if (connection.isClosed()) {
                HTTP2_LOG_INFO("[HttpsServer] 连接已关闭，退出帧循环");
                break;
            }
            
            // 接收帧
            HTTP2_LOG_DEBUG("[HttpsServer] 调用 reader.readFrame()...");
            auto frame_res = co_await reader.readFrame();
            
            if (!frame_res) {
                HTTP2_LOG_ERROR("[HttpsServer] ✗ 读取帧失败: {}", frame_res.error().message());
                if (callbacks.on_error) {
                    callbacks.on_error(connection, frame_res.error());
                }
                break;
            }
            
            HTTP2_LOG_DEBUG("[HttpsServer] ✓ readFrame() 成功返回");
            
            auto frame = frame_res.value();
            frame_count++;
            
            HTTP2_LOG_INFO("[HttpsServer] 📨 收到帧 #{}: type={}, stream={}, length={} bytes, flags=0x{:02X}",
                          frame_count,
                          http2FrameTypeToString(frame->type()),
                          frame->streamId(),
                          frame->length(),
                          frame->flags());
            
            bool should_continue = true;
            
            // 根据帧类型分发到对应的回调
            HTTP2_LOG_DEBUG("[HttpsServer] Processing frame type: {} (raw={})", 
                           http2FrameTypeToString(frame->type()), static_cast<int>(frame->type()));
            
            switch (frame->type()) {
                case Http2FrameType::HEADERS: {
                    auto headers_frame = std::dynamic_pointer_cast<Http2HeadersFrame>(frame);
                    if (headers_frame && callbacks.on_headers) {
                        auto headers_vec_res = headers_frame->decodeHeaders(hpack_decoder);
                        if (headers_vec_res) {
                            // 转换 vector<HpackHeaderField> 到 map<string, string>
                            std::map<std::string, std::string> headers_map;
                            for (const auto& field : headers_vec_res.value()) {
                                headers_map[field.name] = field.value;
                            }
                            
                            bool end_stream = headers_frame->endStream();
                            HTTP2_LOG_INFO("[HttpsServer] HEADERS frame on stream {}, end_stream={}", 
                                          headers_frame->streamId(), end_stream);
                            
                            // 调用回调并等待完成
                            AsyncWaiter<void, Http2Error> callback_waiter;
                            auto callback_coro = callbacks.on_headers(connection, 
                                                                      headers_frame->streamId(),
                                                                      headers_map,
                                                                      end_stream);
                            callback_coro.then([&callback_waiter](){
                                callback_waiter.notify({});
                            });
                            callback_waiter.appendTask(std::move(callback_coro));
                            co_await callback_waiter.wait();
                        } else {
                            HTTP2_LOG_ERROR("[HttpsServer] Failed to decode headers: {}", headers_vec_res.error().message());
                            if (callbacks.on_error) {
                                callbacks.on_error(connection, headers_vec_res.error());
                            }
                            should_continue = false;
                        }
                    }
                    break;
                }
                
                case Http2FrameType::DATA: {
                    HTTP2_LOG_DEBUG("[HttpsServer] 进入 DATA 帧处理分支");
                    auto data_frame = std::dynamic_pointer_cast<Http2DataFrame>(frame);
                    HTTP2_LOG_DEBUG("[HttpsServer] data_frame cast: {}, callbacks.on_data: {}", 
                                   data_frame ? "OK" : "FAIL", callbacks.on_data ? "SET" : "NULL");
                    
                    if (data_frame && callbacks.on_data) {
                        HTTP2_LOG_DEBUG("[HttpsServer] 调用 on_data 回调...");
                        bool end_stream = data_frame->endStream();
                        HTTP2_LOG_INFO("[HttpsServer] DATA frame on stream {}, length={}, end_stream={}", 
                                      data_frame->streamId(), data_frame->data().size(), end_stream);
                        
                        AsyncWaiter<void, Http2Error> callback_waiter;
                        auto callback_coro = callbacks.on_data(connection,
                                                               data_frame->streamId(),
                                                               data_frame->data(),
                                                               end_stream);
                        callback_coro.then([&callback_waiter](){
                            callback_waiter.notify({});
                        });
                        callback_waiter.appendTask(std::move(callback_coro));
                        co_await callback_waiter.wait();
                        HTTP2_LOG_DEBUG("[HttpsServer] on_data 回调完成");
                    } else {
                        if (!data_frame) {
                            HTTP2_LOG_ERROR("[HttpsServer] ✗ DATA frame cast 失败！");
                        }
                        if (!callbacks.on_data) {
                            HTTP2_LOG_ERROR("[HttpsServer] ✗ on_data 回调未设置！");
                        }
                    }
                    break;
                }
                
                case Http2FrameType::SETTINGS: {
                    auto settings_frame = std::dynamic_pointer_cast<Http2SettingsFrame>(frame);
                    if (settings_frame) {
                        bool is_ack = settings_frame->isAck();
                        HTTP2_LOG_INFO("[HttpsServer] SETTINGS frame, ack={}", is_ack);
                        
                        if (callbacks.on_settings) {
                            AsyncWaiter<void, Http2Error> callback_waiter;
                            auto callback_coro = callbacks.on_settings(connection,
                                                                       settings_frame->settings(),
                                                                       is_ack);
                            callback_coro.then([&callback_waiter](){
                                callback_waiter.notify({});
                            });
                            callback_waiter.appendTask(std::move(callback_coro));
                            co_await callback_waiter.wait();
                        }
                        
                        // 如果不是 ACK，自动回复 SETTINGS ACK
                        if (!is_ack) {
                            HTTP2_LOG_DEBUG("[HttpsServer] Sending SETTINGS ACK");
                            auto ack_res = co_await writer.sendSettingsAck();
                            if (!ack_res) {
                                HTTP2_LOG_ERROR("[HttpsServer] Failed to send SETTINGS ACK: {}", ack_res.error().message());
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
                        HTTP2_LOG_INFO("[HttpsServer] PING frame, ack={}, data={}", is_ack, ping_data);
                        
                        if (callbacks.on_ping) {
                            AsyncWaiter<void, Http2Error> callback_waiter;
                            auto callback_coro = callbacks.on_ping(connection, ping_data, is_ack);
                            callback_coro.then([&callback_waiter](){
                                callback_waiter.notify({});
                            });
                            callback_waiter.appendTask(std::move(callback_coro));
                            co_await callback_waiter.wait();
                        }
                        
                        // 如果不是 ACK，自动回复 PING ACK
                        if (!is_ack) {
                            HTTP2_LOG_DEBUG("[HttpsServer] Sending PING ACK");
                            auto pong_res = co_await writer.sendPing(ping_data, true);
                            if (!pong_res) {
                                HTTP2_LOG_ERROR("[HttpsServer] Failed to send PING ACK: {}", pong_res.error().message());
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
                        HTTP2_LOG_INFO("[HttpsServer] GOAWAY frame, last_stream_id={}, error_code={}",
                                      goaway_frame->lastStreamId(),
                                      http2ErrorCodeToString(goaway_frame->errorCode()));
                        
                        if (callbacks.on_goaway) {
                            AsyncWaiter<void, Http2Error> callback_waiter;
                            auto callback_coro = callbacks.on_goaway(connection,
                                                                     goaway_frame->lastStreamId(),
                                                                     goaway_frame->errorCode(),
                                                                     goaway_frame->debugData());
                            callback_coro.then([&callback_waiter](){
                                callback_waiter.notify({});
                            });
                            callback_waiter.appendTask(std::move(callback_coro));
                            co_await callback_waiter.wait();
                        }
                        // GOAWAY 表示连接将关闭
                        should_continue = false;
                    }
                    break;
                }
                
                case Http2FrameType::WINDOW_UPDATE: {
                    auto window_frame = std::dynamic_pointer_cast<Http2WindowUpdateFrame>(frame);
                    if (window_frame) {
                        HTTP2_LOG_DEBUG("[HttpsServer] WINDOW_UPDATE frame, stream_id={}, increment={}",
                                       window_frame->streamId(), window_frame->windowSizeIncrement());
                        
                        if (callbacks.on_window_update) {
                            AsyncWaiter<void, Http2Error> callback_waiter;
                            auto callback_coro = callbacks.on_window_update(connection,
                                                                            window_frame->streamId(),
                                                                            window_frame->windowSizeIncrement());
                            callback_coro.then([&callback_waiter](){
                                callback_waiter.notify({});
                            });
                            callback_waiter.appendTask(std::move(callback_coro));
                            co_await callback_waiter.wait();
                        }
                    }
                    break;
                }
                
                case Http2FrameType::RST_STREAM: {
                    auto rst_frame = std::dynamic_pointer_cast<Http2RstStreamFrame>(frame);
                    if (rst_frame) {
                        HTTP2_LOG_INFO("[HttpsServer] RST_STREAM frame, stream_id={}, error_code={}",
                                      rst_frame->streamId(),
                                      http2ErrorCodeToString(rst_frame->errorCode()));
                        
                        if (callbacks.on_rst_stream) {
                            AsyncWaiter<void, Http2Error> callback_waiter;
                            auto callback_coro = callbacks.on_rst_stream(connection,
                                                                         rst_frame->streamId(),
                                                                         rst_frame->errorCode());
                            callback_coro.then([&callback_waiter](){
                                callback_waiter.notify({});
                            });
                            callback_waiter.appendTask(std::move(callback_coro));
                            co_await callback_waiter.wait();
                        }
                    }
                    break;
                }
                
                case Http2FrameType::PRIORITY: {
                    auto priority_frame = std::dynamic_pointer_cast<Http2PriorityFrame>(frame);
                    if (priority_frame) {
                        HTTP2_LOG_DEBUG("[HttpsServer] PRIORITY frame, stream_id={}", priority_frame->streamId());
                        
                        if (callbacks.on_priority) {
                            AsyncWaiter<void, Http2Error> callback_waiter;
                            auto callback_coro = callbacks.on_priority(connection,
                                                                       priority_frame->streamId(),
                                                                       priority_frame->streamDependency(),
                                                                       priority_frame->weight(),
                                                                       priority_frame->exclusive());
                            callback_coro.then([&callback_waiter](){
                                callback_waiter.notify({});
                            });
                            callback_waiter.appendTask(std::move(callback_coro));
                            co_await callback_waiter.wait();
                        }
                    }
                    break;
                }
                
                default:
                    HTTP2_LOG_WARN("[HttpsServer] Unhandled frame type: {}", 
                                  http2FrameTypeToString(frame->type()));
                    break;
            }
            
            if (!should_continue) {
                HTTP2_LOG_INFO("[HttpsServer] Callback requested connection close");
                break;
            }
        }
        
        HTTP2_LOG_INFO("========================================");
        HTTP2_LOG_INFO("[HttpsServer] 🛑 帧处理循环结束，共处理 {} 个帧", frame_count);
        co_return nil();
    }

    HttpsServerBuilder::HttpsServerBuilder(const std::string& cert_file, const std::string& key_file)
        : m_cert(cert_file), m_key(key_file)
    {
    }

    HttpsServerBuilder& HttpsServerBuilder::addListen(const Host& host)
    {
        m_host = host;
        return *this;
    }

    HttpsServerBuilder &HttpsServerBuilder::threads(int threads)
    {
        m_threads = threads;
        return *this;
    }
    
    HttpsServerBuilder& HttpsServerBuilder::enableHttp2(bool enabled)
    {
        m_enable_http2 = enabled;
        return *this;
    }

    HttpsServer HttpsServerBuilder::build()
    {
        // 步骤1: 创建服务器
        TcpSslServerBuilder builder(m_cert, m_key);
        auto server = builder.backlog(DEFAULT_TCP_BACKLOG_SIZE)
                            .addListen(m_host)
                            .build();
        
        // 步骤2: 主动初始化 SSL 上下文
        HTTPS_LOG_DEBUG("[HttpsServerBuilder] 初始化 SSL 上下文...");
        if (!server.initializeSSLContext()) {
            HTTPS_LOG_WARN("[HttpsServerBuilder] SSL 上下文已初始化或初始化失败");
        }
        
        // 步骤3: 获取 SSL 上下文并配置 ALPN
        SSL_CTX* ctx = server.getSSLContext();
        if (ctx) {
            // 配置 ALPN
            if (m_enable_http2) {
                // 配置服务器支持 HTTP/2 和 HTTP/1.1（HTTP/2 优先）
                AlpnProtocolList alpn_list = AlpnProtocolList::http2WithFallback();
                if (configureServerAlpn(ctx, alpn_list)) {
                    HTTPS_LOG_INFO("[HttpsServerBuilder] ALPN configured: h2, http/1.1");
                } else {
                    HTTPS_LOG_WARN("[HttpsServerBuilder] Failed to configure ALPN");
                }
            } else {
                // 仅支持 HTTP/1.1
                AlpnProtocolList alpn_list = AlpnProtocolList::http11Only();
                configureServerAlpn(ctx, alpn_list);
                HTTPS_LOG_INFO("[HttpsServerBuilder] ALPN configured: http/1.1 only");
            }
        } else {
            HTTPS_LOG_ERROR("[HttpsServerBuilder] Cannot get SSL_CTX!");
        }
        
        return HttpsServer(std::move(server), m_cert, m_key, m_enable_http2);
    }
}

