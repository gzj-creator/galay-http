#include "HttpConnection.h"

namespace galay::http 
{
    HttpConnection::HttpConnection(AsyncTcpSocket &&socket, TimerGenerator&& generator)
        : m_socket(std::move(socket)), m_generator(std::move(generator))
    {
    }

    HttpConnection::HttpConnection(HttpConnection &&other)
        : m_socket(std::move(other.m_socket)), m_generator(std::move(other.m_generator))
    {
    }

    HttpReader HttpConnection::getRequestReader(const HttpParams& params)
    {
        return HttpReader(m_socket, m_generator, params);
    }

    HttpWriter HttpConnection::getResponseWriter(const HttpParams& params)
    {
        return HttpWriter(m_socket, m_generator, params);
    }

    AsyncResult<std::expected<void, CommonError>> HttpConnection::close()
    {
        return m_socket.close();
    }
}