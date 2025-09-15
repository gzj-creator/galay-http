#ifndef GALAY_HTTP_CONNECTION_H
#define GALAY_HTTP_CONNECTION_H 

#include "HttpReader.h"

namespace galay::http
{ 
    class HttpConnection 
    {
    public:
        HttpConnection(AsyncTcpSocket&& socket, Runtime& runtime, size_t id, HttpParams params);

        HttpRequestReader getRequestReader();
        
        ~HttpConnection() = default;
    private:
        AsyncTcpSocket m_socket;
        Runtime& m_runtime;
        size_t m_id;
        HttpParams m_params;
    };
}


#endif