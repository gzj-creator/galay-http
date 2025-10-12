#ifndef GALAY_HTTP_SERVER_H
#define GALAY_HTTP_SERVER_H 

#include <galay/kernel/server/TcpServer.h>
#include "galay-http/kernel/HttpRouter.h"


namespace galay::http 
{
    class HttpServer 
    {
    public:
        using HttpConnFunc = std::function<Coroutine<nil>(HttpConnection)>;
        HttpServer(TcpServer&& tcpServer) 
            : m_server(std::move(tcpServer)) {}
        void listen(const Host& host);
        void run(Runtime& runtime, const HttpConnFunc& handler);
        void run(Runtime& runtime, HttpRouter& router, HttpSettings params = HttpSettings());
        void wait();
        void stop();
    private:
        Coroutine<nil> handleConnection(Runtime& runtime, HttpRouter& router, HttpSettings params, AsyncTcpSocket socket);
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