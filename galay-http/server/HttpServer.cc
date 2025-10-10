#include "HttpServer.h"

namespace galay::http
{
    HttpServer::HttpServer(TcpServer&& server)
        : m_server(std::move(server))
    {
    }

    void HttpServer::listen(const Host &host)
    {
        m_server.listenOn(host, DEFAULT_TCP_BACKLOG_SIZE);
    }

    void HttpServer::run(std::function<Coroutine<nil>(HttpConnection, AsyncFactory)> handler)
    {
        m_server.run([handler](AsyncTcpSocket socket, AsyncFactory factory) -> Coroutine<nil> { 
            HttpConnection conn(std::move(socket), factory.createTimerGenerator());
            return handler(std::move(conn), factory);
        });
    }

    void HttpServer::run(HttpRouter router, HttpParams params)
    {
        m_server.run([router, params](AsyncTcpSocket socket, AsyncFactory factory) -> Coroutine<nil> { 
            HttpConnection conn(std::move(socket), factory.createTimerGenerator());
            auto reader = conn.getRequestReader(params);
            co_await reader.getRequest();
        });
    }

    void HttpServer::stop()
    {
        m_server.stop();
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
                            .threads(m_threads)
                            .timeout(-1)
                            .startCoChecker(m_coCheckerInterval)
                            .build();
        HttpParams params;
        return HttpServer(std::move(server));
    }
}