#include "WsConnection.h"
#include "galay-http/utils/HttpLogger.h"

namespace galay::http
{
    WsConnection WsConnection::from(HttpConnection& httpConnection)
    {
        HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[{}] [WsConnection] Creating WsConnection from HttpConnection", __LINE__);
        return WsConnection(httpConnection);
    }

    WsConnection::WsConnection(HttpConnection& httpConnection)
        : m_connection(httpConnection)
    {
        HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[{}] [WsConnection] WsConnection constructed", __LINE__);
    }

    WsReader WsConnection::getReader(const WsSettings& params)
    {
        HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[{}] [WsConnection] Creating WsReader", __LINE__);
        return WsReader(m_connection.m_socket, m_connection.m_generator, params);
    }

    WsWriter WsConnection::getWriter(const WsSettings& params)
    {
        HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[{}] [WsConnection] Creating WsWriter", __LINE__);
        return WsWriter(m_connection.m_socket, m_connection.m_generator, params);
    }

    AsyncResult<std::expected<void, CommonError>> WsConnection::close()
    {
        HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[{}] [WsConnection] Closing connection", __LINE__);
        return m_connection.close();
    }

    bool WsConnection::isClosed() const
    {
        return m_connection.isClosed();
    }
}