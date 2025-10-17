#include "WsConnection.h"
#include "galay-http/utils/HttpLogger.h"

namespace galay::http
{
    WsConnection WsConnection::from(HttpConnection& httpConnection)
    {
        HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[WsConnection] Upgrade from HTTP");
        return WsConnection(httpConnection);
    }

    WsConnection::WsConnection(HttpConnection& httpConnection)
        : m_connection(httpConnection)
    {
    }

    WsReader WsConnection::getReader(const WsSettings& params)
    {
        return WsReader(m_connection.m_socket, m_connection.m_generator, params);
    }

    WsWriter WsConnection::getWriter(const WsSettings& params)
    {
        return WsWriter(m_connection.m_socket, m_connection.m_generator, params);
    }

    AsyncResult<std::expected<void, CommonError>> WsConnection::close()
    {
        HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[WsConnection] Close");
        return m_connection.close();
    }

    bool WsConnection::isClosed() const
    {
        return m_connection.isClosed();
    }
}