#ifndef GALAY_HTTP2_CONNECTION_H
#define GALAY_HTTP2_CONNECTION_H

#include "galay-http/kernel/http/HttpConnection.h"
#include "galay-http/kernel/http/HttpsConnection.h"
#include "Http2Reader.h"
#include "Http2Writer.h"
#include "Http2Stream.h"
#include <variant>
#include <functional>

namespace galay::http
{
    /**
     * @brief HTTP/2 连接
     * 
     * 类似于 WsConnection，封装 HTTP/2 连接的读写操作
     * 支持 HTTP（AsyncTcpSocket）和 HTTPS（AsyncSslSocket）
     */
    class Http2Connection
    {
    public:
        static Http2Connection from(HttpConnection& httpConnection, const Http2Settings& settings = Http2Settings{});
        static Http2Connection from(HttpsConnection& httpsConnection, const Http2Settings& settings = Http2Settings{});
        Http2Connection(HttpConnection& httpConnection, const Http2Settings& settings = Http2Settings{});
        Http2Connection(HttpsConnection& httpsConnection, const Http2Settings& settings = Http2Settings{});
        
        // 获取读写器
        Http2Reader getReader(const Http2Settings& params);
        Http2Writer getWriter(const Http2Settings& params);
        
        // 获取流管理器
        Http2StreamManager& streamManager() { return m_stream_manager; }
        
        // 关闭连接
        AsyncResult<std::expected<void, CommonError>> close();
        
        bool isClosed() const;
        
        ~Http2Connection() = default;
        
    private:
        // 使用 variant 存储两种连接类型的引用包装器
        std::variant<
            std::reference_wrapper<HttpConnection>,
            std::reference_wrapper<HttpsConnection>
        > m_connection;
        Http2StreamManager m_stream_manager;
    };
}

#endif // GALAY_HTTP2_CONNECTION_H

