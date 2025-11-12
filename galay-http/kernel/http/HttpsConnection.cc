#include "HttpsConnection.h"
#include "HttpsReader.h"
#include "HttpsWriter.h"
#include "galay-http/utils/HttpsDebugLog.h"
#include "galay-http/protoc/alpn/AlpnProtocol.h"
#include "galay/kernel/coroutine/CoSchedulerHandle.hpp"
#include <openssl/ssl.h>

namespace galay::http
{
    HttpsConnection::HttpsConnection(AsyncSslSocket&& socket, CoSchedulerHandle handle)
        : m_socket(std::move(socket)), m_handle(handle)
    {
        HTTPS_LOG_DEBUG("[HttpsConnection] Created");
    }

    HttpsConnection::HttpsConnection(HttpsConnection&& other)
        : m_socket(std::move(other.m_socket)), m_handle(other.m_handle), 
          m_params(std::move(other.m_params)), m_is_closed(other.m_is_closed)
    {
    }

    HttpsReader HttpsConnection::getRequestReader(const HttpSettings& params)
    {
        HTTPS_LOG_DEBUG("[HttpsConnection] Creating request reader");
        return HttpsReader(m_socket, m_handle, params);
    }

    HttpsWriter HttpsConnection::getResponseWriter(const HttpSettings& params)
    {
        HTTPS_LOG_DEBUG("[HttpsConnection] Creating response writer");
        return HttpsWriter(m_socket, m_handle, params);
    }

    AsyncResult<std::expected<void, CommonError>> HttpsConnection::close()
    {
        if (m_is_closed) {
            HTTPS_LOG_DEBUG("[HttpsConnection] Already closed, skipping");
            return {std::expected<void, CommonError>()};
        }
        HTTPS_LOG_DEBUG("[HttpsConnection] Closing connection");
        m_is_closed = true;
        return m_socket.sslClose();
    }

    bool HttpsConnection::isClosed() const
    {
        return m_is_closed;
    }

    void HttpsConnection::markClosed()
    {
        m_is_closed = true;
    }
    
    std::string HttpsConnection::getAlpnProtocol() const
    {
        SSL* ssl = getSSL();
        if (!ssl) {
            return "";
        }
        return galay::http::getAlpnProtocol(ssl);
    }
    
    bool HttpsConnection::isHttp2() const
    {
        return getAlpnProtocol() == "h2";
    }
    
    SSL* HttpsConnection::getSSL() const
    {
        return m_socket.getSsl();
    }
}

