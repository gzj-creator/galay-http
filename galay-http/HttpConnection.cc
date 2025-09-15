#include "HttpConnection.h"

namespace galay::http 
{
    HttpConnection::HttpConnection(AsyncTcpSocket &&socket, Runtime &runtime, size_t id, HttpParams params)
        : m_socket(std::move(socket)), m_runtime(runtime), m_id(id), m_params(params)
    {
    }

    HttpRequestReader HttpConnection::getRequestReader()
    {
        return HttpRequestReader(m_socket, m_runtime, m_id, m_params);
    }
}