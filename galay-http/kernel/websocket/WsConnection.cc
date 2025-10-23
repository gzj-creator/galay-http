#include "WsConnection.h"
#include "galay-http/utils/WsDebugLog.h"

namespace galay::http
{
    WsConnection WsConnection::from(HttpConnection& httpConnection)
    {
        WS_LOG_DEBUG("[WsConnection] Upgrade from HTTP");
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
        WS_LOG_DEBUG("[WsConnection] Close");
        return m_connection.close();
    }

    bool WsConnection::isClosed() const
    {
        return m_connection.isClosed();
    }
}