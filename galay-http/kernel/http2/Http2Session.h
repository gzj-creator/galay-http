#ifndef GALAY_HTTP2_SESSION_H
#define GALAY_HTTP2_SESSION_H

#include "Http2Reader.h"
#include "Http2Writer.h"
#include "Http2Conn.h"
#include "galay-kernel/async/TcpSocket.h"

namespace galay::http2
{

using namespace galay::async;
using namespace galay::kernel;

/**
 * @brief HTTP/2 会话模板类
 * @details 持有 Http2Conn、reader 和 writer，负责 HTTP/2 通信
 */
template<typename SocketType>
class Http2SessionImpl
{
public:
    Http2SessionImpl(Http2ConnImpl<SocketType>& conn)
        : m_conn(conn)
        , m_reader(conn)
        , m_writer(conn)
    {
    }

    Http2ReaderImpl<SocketType>& getReader() {
        return m_reader;
    }

    Http2WriterImpl<SocketType>& getWriter() {
        return m_writer;
    }

    Http2ConnImpl<SocketType>& getConn() {
        return m_conn;
    }

    // 便捷方法：发送 HEADERS 帧
    SendHeadersAwaitableImpl<SocketType> sendHeaders(uint32_t stream_id,
                                                      std::vector<Http2HeaderField> headers,
                                                      bool end_stream = false) {
        return m_writer.sendHeaders(stream_id, std::move(headers), end_stream);
    }

    // 便捷方法：发送 DATA 帧
    SendDataAwaitableImpl<SocketType> sendData(uint32_t stream_id,
                                                std::string data,
                                                bool end_stream = false) {
        return m_writer.sendData(stream_id, std::move(data), end_stream);
    }

    // 便捷方法：发送完整的 HTTP/2 请求（用于客户端）
    SendRequestAwaitableImpl<SocketType> sendRequest(uint32_t stream_id, Http2Request request) {
        return m_writer.sendRequest(stream_id, std::move(request));
    }

    // 便捷方法：发送完整的 HTTP/2 响应（用于服务器）
    SendResponseAwaitableImpl<SocketType> sendResponse(uint32_t stream_id, Http2Response response) {
        return m_writer.sendResponse(stream_id, std::move(response));
    }

    // 便捷方法：读取一个 HTTP/2 帧
    GetFrameAwaitableImpl<SocketType> getFrame() {
        return m_reader.getFrame();
    }

    // 便捷方法：读取完整的 HTTP/2 响应（用于客户端）
    GetResponseAwaitableImpl<SocketType> getResponse(uint32_t stream_id, Http2Response& response) {
        return m_reader.getResponse(stream_id, response);
    }

private:
    Http2ConnImpl<SocketType>& m_conn;
    Http2ReaderImpl<SocketType> m_reader;
    Http2WriterImpl<SocketType> m_writer;
};

// 类型别名 - HTTP/2 over TCP
using Http2Session = Http2SessionImpl<galay::async::TcpSocket>;

} // namespace galay::http2

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/SslSocket.h"

namespace galay::http2 {

// 类型别名 - HTTP/2 over SSL
using Http2SslSession = Http2SessionImpl<galay::ssl::SslSocket>;

} // namespace galay::http2
#endif

#endif // GALAY_HTTP2_SESSION_H
