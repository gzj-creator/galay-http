#ifndef GALAY_HTTP_CONNECTION_H
#define GALAY_HTTP_CONNECTION_H 

#include "HttpReader.h"
#include "HttpWriter.h"

namespace galay::http
{ 
    class WsConnection;
    class HttpConnection 
    {
        friend class WsConnection;
    public:
        HttpConnection(AsyncTcpSocket&& socket, TimerGenerator&& generator);
        HttpConnection(HttpConnection&& other);

        HttpReader getRequestReader(const HttpSettings& params);
        HttpWriter getResponseWriter(const HttpSettings& params);
        
        AsyncResult<std::expected<void, CommonError>> close();
        bool isClosed() const;
        // 标记连接为已关闭，不执行任何 I/O 操作（用于连接已被对端关闭的情况）
        void markClosed();
        ~HttpConnection() = default;
    private:
        bool m_is_closed = false;
        AsyncTcpSocket m_socket;
        TimerGenerator m_generator;
        std::unordered_map<std::string, std::string> m_params;
    };
}


#endif