#ifndef GALAY_WS_CONN_H
#define GALAY_WS_CONN_H

#include "WsReader.h"
#include "WsWriter.h"
#include "galay-http/kernel/http/HttpConn.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/common/Buffer.h"

namespace galay::websocket
{

using namespace galay::async;
using namespace galay::kernel;

/**
 * @brief WebSocket连接模板类
 */
template<typename SocketType>
class WsConnImpl
{
public:
    WsConnImpl(galay::http::HttpConnImpl<SocketType>&& http_conn,
               const WsReaderSetting& reader_setting,
               WsWriterSetting writer_setting,
               bool is_server = true)
        : m_socket(std::move(http_conn.m_socket))
        , m_ring_buffer(std::move(http_conn.m_ring_buffer))
        , m_reader_setting(reader_setting)
        , m_writer_setting(writer_setting)
        , m_is_server(is_server)
        , m_writer(m_writer_setting, m_socket)
        , m_reader(m_ring_buffer, m_reader_setting, m_socket, is_server, writer_setting.use_mask)
    {
        m_writer_setting.use_mask = !is_server;
    }

    WsConnImpl(SocketType&& socket,
               RingBuffer&& ring_buffer,
               const WsReaderSetting& reader_setting,
               WsWriterSetting writer_setting,
               bool is_server = true)
        : m_socket(std::move(socket))
        , m_ring_buffer(std::move(ring_buffer))
        , m_reader_setting(reader_setting)
        , m_writer_setting(writer_setting)
        , m_is_server(is_server)
        , m_writer(m_writer_setting, m_socket)
        , m_reader(m_ring_buffer, m_reader_setting, m_socket, is_server, writer_setting.use_mask)
    {
        m_writer_setting.use_mask = !is_server;
    }

    ~WsConnImpl() = default;

    WsConnImpl(const WsConnImpl&) = delete;
    WsConnImpl& operator=(const WsConnImpl&) = delete;
    WsConnImpl(WsConnImpl&&) = delete;
    WsConnImpl& operator=(WsConnImpl&&) = delete;

    auto close() {
        return m_socket.close();
    }

    SocketType& socket() { return m_socket; }
    RingBuffer& ringBuffer() { return m_ring_buffer; }

    WsReaderImpl<SocketType>& getReader() {
        return m_reader;
    }

    WsWriterImpl<SocketType>& getWriter() {
        return m_writer;
    }

    bool isServer() const { return m_is_server; }

private:
    SocketType m_socket;
    RingBuffer m_ring_buffer;
    WsReaderSetting m_reader_setting;
    WsWriterSetting m_writer_setting;
    bool m_is_server;
    WsWriterImpl<SocketType> m_writer;
    WsReaderImpl<SocketType> m_reader;
};

// 类型别名 - WebSocket over TCP
using WsConn = WsConnImpl<TcpSocket>;

} // namespace galay::websocket

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/SslSocket.h"
namespace galay::websocket {
using WssConn = WsConnImpl<galay::ssl::SslSocket>;
} // namespace galay::websocket
#endif

#endif // GALAY_WS_CONN_H
