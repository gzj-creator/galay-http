#include "HttpServer.h"
#include "galay-http/kernel/http/HttpParams.hpp"
#include "galay-http/utils/HttpUtils.h"
#include "galay-http/utils/HttpDebugLog.h"
#include "galay/kernel/coroutine/CoSchedulerHandle.hpp"
#include "galay/kernel/runtime/Runtime.h"

namespace galay::http
{

    void HttpServer::listen(const Host &host)
    {
        m_server.listenOn(host, DEFAULT_TCP_BACKLOG_SIZE);
    }

    void HttpServer::run(Runtime& runtime, const HttpConnFunc& handler)
    {
        m_server.run(runtime, [handler](AsyncTcpSocket socket, CoSchedulerHandle handle) -> Coroutine<nil> { 
            HttpConnection conn(std::move(socket), handle);
            return handler(std::move(conn));
        });
    }

    void HttpServer::run(Runtime& runtime, HttpRouter& router, HttpSettings params)
    {
        m_server.run(runtime, [this, prouter = &router, params](AsyncTcpSocket socket, CoSchedulerHandle handle) -> Coroutine<nil> {
            return handleConnection(handle, *prouter, params, std::move(socket));
        });
    }

    void HttpServer::stop()
    {
        m_server.stop();
    }

    void HttpServer::wait()
    {
        m_server.wait();
    }

    Coroutine<nil> HttpServer::handleConnection(CoSchedulerHandle handle, HttpRouter& router, HttpSettings params, AsyncTcpSocket socket)
    {
        HttpConnection conn(std::move(socket), handle);
        
        HTTP_LOG_DEBUG("[HttpServer] New connection");
        
        while(true) 
        {
            // 检查连接状态
            if (conn.isClosed()) {
                HTTP_LOG_DEBUG("[HttpServer] Connection already closed");
                co_return nil();
            }
            
            auto reader = conn.getRequestReader(params);
            auto writer = conn.getResponseWriter(params);
            
            auto request_res = co_await reader.getRequest();
            
            if(!request_res) {
                if(request_res.error().code() == HttpErrorCode::kHttpError_ConnectionClose) {
                    HTTP_LOG_DEBUG("[HttpServer] Connection closed by peer");
                    co_await conn.close();
                    co_return nil();
                }
                HTTP_LOG_DEBUG("[HttpServer] Request error: {}", request_res.error().message());
                auto response = HttpUtils::defaultHttpResponse(request_res.error().toHttpStatusCode());
                response.header().headerPairs().addHeaderPair("Connection", "close");
                auto response_res = co_await writer.reply(response);
                if(!response_res) {
                    HTTP_LOG_ERROR("[HttpServer] Reply error: {}", response_res.error().message());
                } 
                co_await conn.close();
                co_return nil();
            }
            auto& request = request_res.value();
            
            auto route_res = co_await router.route(request, conn);
            
            if(!route_res) {
                HTTP_LOG_DEBUG("[HttpServer] Route error: {}", route_res.error().message());
                auto response = HttpUtils::defaultHttpResponse(route_res.error().toHttpStatusCode());
                auto response_res = co_await writer.reply(response);
                if(!response_res) {
                    co_await conn.close();
                    HTTP_LOG_ERROR("[HttpServer] Reply error: {}", response_res.error().message());
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
                HTTP_LOG_DEBUG("[HttpServer] Connection closed");
                co_return nil();
            }
        }
    }

    HttpServerBuilder& HttpServerBuilder::addListen(const Host& host)
    {
        m_host = host;
        return *this;
    }

    HttpServerBuilder &HttpServerBuilder::startCoChecker(std::chrono::milliseconds interval)
    {
        m_coCheckerInterval = interval;
        return *this;
    }

    HttpServerBuilder &HttpServerBuilder::threads(int threads)
    {
        m_threads = threads;
        return *this;
    }

    HttpServer HttpServerBuilder::build()
    {
        TcpServerBuilder builder;
        auto server = builder.backlog(DEFAULT_TCP_BACKLOG_SIZE)
                            .addListen(m_host)
                            .build();
        return HttpServer(std::move(server));
    }
}