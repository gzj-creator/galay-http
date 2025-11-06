#include "HttpsServer.h"
#include "galay-http/kernel/http/HttpsConnection.h"
#include "galay-http/kernel/http/HttpsReader.h"
#include "galay-http/kernel/http/HttpsWriter.h"
#include "galay-http/kernel/http/HttpParams.hpp"
#include "galay-http/utils/HttpUtils.h"
#include "galay-http/utils/HttpDebugLog.h"
#include "galay-http/utils/HttpsDebugLog.h"
#include "galay/kernel/runtime/Runtime.h"
#include "galay/kernel/async/AsyncFactory.h"
#include "galay/common/Common.h"

namespace galay::http
{
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
        HTTPS_LOG_DEBUG("[HttpsServer] run() with router (HTTPS + HTTP/1.x)");
        
        m_server.run(runtime, [this, &runtime, &router, params](AsyncSslSocket socket) -> Coroutine<nil> {
            HTTPS_LOG_DEBUG("[HttpsServer] New SSL connection accepted");
            return handleConnection(runtime, router, params, std::move(socket));
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
        
        HTTPS_LOG_DEBUG("[HttpsServer] New HTTPS + HTTP/1.x connection");
        
        while(true) 
        {
            // 检查连接状态
            if (conn.isClosed()) {
                HTTPS_LOG_DEBUG("[HttpsServer] Connection already closed");
                co_return nil();
            }
            
            auto reader = conn.getRequestReader(params);
            auto writer = conn.getResponseWriter(params);
            
            HTTPS_LOG_DEBUG("[HttpsServer] Waiting for HTTP request...");
            auto request_res = co_await reader.getRequest();
            
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
    
    // HttpsServerBuilder Implementation

    HttpsServerBuilder::HttpsServerBuilder(const std::string& cert_file, const std::string& key_file)
        : m_cert(cert_file), m_key(key_file)
    {
    }

    HttpsServerBuilder& HttpsServerBuilder::addListen(const Host& host)
    {
        m_host = host;
        return *this;
    }

    HttpsServerBuilder& HttpsServerBuilder::threads(int threads)
    {
        m_threads = threads;
        return *this;
    }

    HttpsServer HttpsServerBuilder::build()
    {
        HTTPS_LOG_DEBUG("[HttpsServerBuilder] Building HTTPS server (HTTP/1.x only)");
        
        // 创建 TcpSslServer
        TcpSslServerBuilder builder(m_cert, m_key);
        auto server = builder.backlog(DEFAULT_TCP_BACKLOG_SIZE)
                            .addListen(m_host)
                            .build();
        
        HTTPS_LOG_INFO("[HttpsServerBuilder] HTTPS server created for {}:{}", m_host.ip, m_host.port);
        
        return HttpsServer(std::move(server), m_cert, m_key);
    }
}
