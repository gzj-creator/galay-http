#ifndef GALAY_HTTP_SERVER_H
#define GALAY_HTTP_SERVER_H 

#include <galay/kernel/server/TcpServer.h>
#include "galay-http/kernel/HttpRouter.h"


namespace galay::http 
{
    class HttpServer 
    {
    public:
        HttpServer(TcpServer&& server);
        void listen(const Host& host);
        void run(std::function<Coroutine<nil>(HttpConnection, AsyncFactory)> handler);
        void run(HttpRouter router, HttpParams params = HttpParams());
        void stop();
    private:
    private:
        TcpServer m_server;
    };


    class HttpServerBuilder 
    {
    public:
        HttpServerBuilder& addListen(const Host& host);
        HttpServerBuilder& startCoChecker(std::chrono::milliseconds interval);
        HttpServerBuilder& threads(int threads);

        HttpServer build();
    private:
        Host m_host = { "0.0.0.0", 8080};
        std::chrono::milliseconds m_coCheckerInterval = std::chrono::milliseconds(-1);
        int m_threads = DEFAULT_COS_SCHEDULER_THREAD_NUM;
    };
}

#endif