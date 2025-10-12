#ifndef GALAY_HTTP_CONNECTION_H
#define GALAY_HTTP_CONNECTION_H 

#include "HttpReader.h"
#include "HttpWriter.h"

namespace galay::http
{ 
    class HttpConnection 
    {
    public:
        HttpConnection(AsyncTcpSocket&& socket, TimerGenerator&& generator);
        HttpConnection(HttpConnection&& other);

        HttpReader getRequestReader(const HttpSettings& params);
        HttpWriter getResponseWriter(const HttpSettings& params);
        
        AsyncResult<std::expected<void, CommonError>> close();
        bool isClosed() const;
        ~HttpConnection() = default;
    private:
        bool m_is_closed = false;
        AsyncTcpSocket m_socket;
        TimerGenerator m_generator;
        std::unordered_map<std::string, std::string> m_params;
    };
}


#endif