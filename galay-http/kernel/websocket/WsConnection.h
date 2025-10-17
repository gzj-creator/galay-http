#ifndef GALAY_HTTP_WS_CONNECTION_H
#define GALAY_HTTP_WS_CONNECTION_H

#include "galay-http/kernel/http/HttpConnection.h"
#include "WsReader.h"
#include "WsWriter.h"

namespace galay::http
{
    class WsConnection
    {
    public:
        static WsConnection from(HttpConnection& httpConnection);
        WsConnection(HttpConnection& httpConnection);

        // 获取读写器
        WsReader getReader(const WsSettings& params);
        WsWriter getWriter(const WsSettings& params);

        // 关闭连接
        AsyncResult<std::expected<void, CommonError>> close();
        
        bool isClosed() const;
        
        ~WsConnection() = default;

    private:
        HttpConnection& m_connection;
    };
}

#endif