#include "HttpServer.h"
#include "galay-http/kernel/HttpParams.hpp"
#include "galay-http/utils/HttpUtils.h"
#include "galay-http/utils/HttpLogger.h"
#include "galay/kernel/runtime/Runtime.h"

namespace galay::http
{

    void HttpServer::listen(const Host &host)
    {
        m_server.listenOn(host, DEFAULT_TCP_BACKLOG_SIZE);
    }

    void HttpServer::run(Runtime& runtime, const HttpConnFunc& handler)
    {
        m_server.run(runtime, [handler, &runtime](AsyncTcpSocket socket) -> Coroutine<nil> { 
            AsyncFactory factory = runtime.getAsyncFactory();
            HttpConnection conn(std::move(socket), factory.getTimerGenerator());
            return handler(std::move(conn));
        });
    }

    void HttpServer::run(Runtime& runtime, HttpRouter& router, HttpSettings params)
    {
        m_server.run(runtime, [this, &runtime, &router, params](AsyncTcpSocket socket) -> Coroutine<nil> {
            return handleConnection(runtime, router, params, std::move(socket));
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

    Coroutine<nil> HttpServer::handleConnection(Runtime& runtime, HttpRouter& router, HttpSettings params, AsyncTcpSocket socket)
    {
        AsyncFactory factory = runtime.getAsyncFactory();
        HttpConnection conn(std::move(socket), factory.getTimerGenerator());
        while(true) 
        {
            auto reader = conn.getRequestReader(params);
            auto writer = conn.getResponseWriter(params);
            auto request_res = co_await reader.getRequest();
            if(!request_res) {
                if(request_res.error().code() == HttpErrorCode::kHttpError_ConnectionClose) {
                    co_await conn.close();
                    co_return nil();
                }
                HttpLogger::getInstance()->getLogger()->getSpdlogger()->error("[getRequest] [{}]", request_res.error().message());
                auto response = HttpUtils::defaultHttpResponse(request_res.error().toHttpStatusCode());
                response.header().headerPairs().addHeaderPair("Connection", "close");
                auto response_res = co_await writer.reply(response);
                if(!response_res) {
                    HttpLogger::getInstance()->getLogger()->getSpdlogger()->error("[RespReply] [{}]", response_res.error().message());
                } 
                co_await conn.close();
                co_return nil();
            }
            auto& request = request_res.value();
            SERVER_REQUEST_LOG(request.header().method(), request.header().uri());
            auto route_res = co_await router.route(request, conn);
            if(!route_res) {
                auto response = HttpUtils::defaultHttpResponse(route_res.error().toHttpStatusCode());
                auto response_res = co_await writer.reply(response);
                if(!response_res) {
                    co_await conn.close();
                    HttpLogger::getInstance()->getLogger()->getSpdlogger()->error("[Resp] [{}]", response_res.error().message());
                    co_return nil();
                }
                continue;
            }
            if(request.header().isConnectionClose()) {
                if(!conn.isClosed()) {
                    co_await conn.close();
                    co_return nil();
                }
            } 
            if (conn.isClosed()) {
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