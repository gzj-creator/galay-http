#include "Http2Connection.h"
#include "galay-http/utils/Http2DebugLog.h"
#include <memory>

namespace galay::http
{
    Http2Connection Http2Connection::from(HttpConnection& httpConnection)
    {
        HTTP2_LOG_DEBUG("[Http2Connection] Upgrade from HTTP/1.1");
        return Http2Connection(httpConnection);
    }
    
    Http2Connection::Http2Connection(HttpConnection& httpConnection)
        : m_connection(httpConnection)
        , m_stream_manager(Http2Settings{})
    {
    }
    
    Http2Reader Http2Connection::getReader(const Http2Settings& params)
    {
        return Http2Reader(m_connection.m_socket, m_connection.m_generator, m_stream_manager, params);
    }
    
    Http2Writer Http2Connection::getWriter(const Http2Settings& params)
    {
        return Http2Writer(m_connection.m_socket, m_connection.m_generator, m_stream_manager, params);
    }
    
    AsyncResult<std::expected<void, CommonError>> Http2Connection::close()
    {
        HTTP2_LOG_DEBUG("[Http2Connection] Close");
        return m_connection.close();
    }
    
    bool Http2Connection::isClosed() const
    {
        return m_connection.isClosed();
    }
}
