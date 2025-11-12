#include "Http2Connection.h"
#include "galay-http/utils/Http2DebugLog.h"

namespace galay::http
{
    Http2Connection Http2Connection::from(HttpConnection& httpConnection, const Http2Settings& settings)
    {
        HTTP2_LOG_DEBUG("[Http2Connection] Upgrade from HTTP/1.1");
        return Http2Connection(httpConnection, settings);
    }
    
    Http2Connection Http2Connection::from(HttpsConnection& httpsConnection, const Http2Settings& settings)
    {
        HTTP2_LOG_DEBUG("[Http2Connection] Upgrade from HTTPS to HTTP/2");
        return Http2Connection(httpsConnection, settings);
    }
    
    Http2Connection::Http2Connection(HttpConnection& httpConnection, const Http2Settings& settings)
        : m_connection(std::ref(httpConnection))
        , m_stream_manager(settings)
    {
    }
    
    Http2Connection::Http2Connection(HttpsConnection& httpsConnection, const Http2Settings& settings)
        : m_connection(std::ref(httpsConnection))
        , m_stream_manager(settings)
    {
    }
    
    Http2Reader Http2Connection::getReader(const Http2Settings& params)
    {
        return std::visit([&](auto&& conn) -> Http2Reader {
            Http2SocketAdapter adapter(conn.get().m_socket);
            return Http2Reader(adapter, conn.get().m_handle, m_stream_manager, params);
        }, m_connection);
    }
    
    Http2Writer Http2Connection::getWriter(const Http2Settings& params)
    {
        return std::visit([&](auto&& conn) -> Http2Writer {
            Http2SocketAdapter adapter(conn.get().m_socket);
            return Http2Writer(adapter, conn.get().m_handle , m_stream_manager, params);
        }, m_connection);
    }
    
    AsyncResult<std::expected<void, CommonError>> Http2Connection::close()
    {
        HTTP2_LOG_DEBUG("[Http2Connection] Close");
        return std::visit([](auto&& conn) {
            return conn.get().close();
        }, m_connection);
    }
    
    bool Http2Connection::isClosed() const
    {
        return std::visit([](auto&& conn) {
            return conn.get().isClosed();
        }, m_connection);
    }
}
