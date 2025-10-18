#include "HttpConnection.h"
#include "galay/common/Error.h"
#include "galay-http/utils/HttpLogger.h"

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

    HttpReader HttpConnection::getRequestReader(const HttpSettings& params)
    {
        return HttpReader(m_socket, m_generator, params);
    }

    HttpWriter HttpConnection::getResponseWriter(const HttpSettings& params)
    {
        return HttpWriter(m_socket, m_generator, params);
    }

    AsyncResult<std::expected<void, CommonError>> HttpConnection::close()
    {
        if(m_is_closed == true) {
            return {std::expected<void, CommonError>()};
        }
        HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[HttpConnection] Close");
        m_is_closed = true;
        return m_socket.close();
    }

    bool HttpConnection::isClosed() const
    {
        return m_is_closed;
    }

    void HttpConnection::markClosed()
    {
        if (!m_is_closed) {
            HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[HttpConnection] Mark as closed (peer disconnected)");
            m_is_closed = true;
        }
    }
}