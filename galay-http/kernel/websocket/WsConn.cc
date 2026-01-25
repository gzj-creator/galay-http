#include "WsConn.h"
#include "galay-http/kernel/http/HttpConn.h"

namespace galay::websocket
{

WsConn::WsConn(galay::http::HttpConn&& http_conn,
               const WsReaderSetting& reader_setting,
               WsWriterSetting writer_setting,
               bool is_server)
    : m_socket(std::move(http_conn.m_socket))
    , m_ring_buffer(std::move(http_conn.m_ring_buffer))
    , m_reader_setting(reader_setting)
    , m_writer_setting(writer_setting)
    , m_is_server(is_server)
    , m_writer(m_writer_setting, m_socket)
    , m_reader(m_ring_buffer, m_reader_setting, m_socket, is_server, writer_setting.use_mask)
{
    // 根据 WebSocket 协议自动设置 use_mask：
    // - 客户端发送的帧必须使用掩码（RFC 6455 Section 5.1）
    // - 服务器发送的帧不得使用掩码（RFC 6455 Section 5.1）
    m_writer_setting.use_mask = !is_server;
}

} // namespace galay::websocket
