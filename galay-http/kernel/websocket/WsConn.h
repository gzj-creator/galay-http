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
 * @tparam SocketType Socket类型（TcpSocket 或 SslSocket）
 * @details 封装WebSocket连接的底层资源，不持有reader/writer，通过接口构造返回
 */
template<typename SocketType>
class WsConnImpl
{
public:
    /**
     * @brief 从HttpConn构造（用于升级场景）
     */
    WsConnImpl(galay::http::HttpConnImpl<SocketType>&& http_conn, bool is_server = true)
        : m_socket(std::move(http_conn.m_socket))
        , m_ring_buffer(std::move(http_conn.m_ring_buffer))
        , m_is_server(is_server)
    {
    }

    /**
     * @brief 直接构造
     */
    WsConnImpl(SocketType&& socket, RingBuffer&& ring_buffer, bool is_server = true)
        : m_socket(std::move(socket))
        , m_ring_buffer(std::move(ring_buffer))
        , m_is_server(is_server)
    {
    }

    /**
     * @brief 构造函数（只持有socket）
     */
    WsConnImpl(SocketType&& socket, bool is_server = true)
        : m_socket(std::move(socket))
        , m_ring_buffer(8192)  // 默认8KB buffer
        , m_is_server(is_server)
    {
    }

    ~WsConnImpl() = default;

    // 禁用拷贝
    WsConnImpl(const WsConnImpl&) = delete;
    WsConnImpl& operator=(const WsConnImpl&) = delete;

    // 启用移动
    WsConnImpl(WsConnImpl&&) = default;
    WsConnImpl& operator=(WsConnImpl&&) = default;

    /**
     * @brief 关闭连接
     */
    auto close() {
        return m_socket.close();
    }

    /**
     * @brief 获取底层Socket引用
     */
    SocketType& socket() { return m_socket; }

    /**
     * @brief 获取RingBuffer引用
     */
    RingBuffer& ringBuffer() { return m_ring_buffer; }

    /**
     * @brief 获取WsReader
     * @param setting WsReaderSetting配置
     * @return WsReaderImpl<SocketType> Reader对象
     */
    WsReaderImpl<SocketType> getReader(const WsReaderSetting& setting = WsReaderSetting()) {
        // use_mask: 客户端需要mask，服务器不需要
        bool use_mask = !m_is_server;
        return WsReaderImpl<SocketType>(m_ring_buffer, setting, m_socket, m_is_server, use_mask);
    }

    /**
     * @brief 获取WsWriter
     * @param setting WsWriterSetting配置
     * @return WsWriterImpl<SocketType> Writer对象
     */
    WsWriterImpl<SocketType> getWriter(WsWriterSetting setting = WsWriterSetting()) {
        // 客户端需要mask，服务器不需要
        setting.use_mask = !m_is_server;
        return WsWriterImpl<SocketType>(setting, m_socket);
    }

    /**
     * @brief 是否为服务器端连接
     */
    bool isServer() const { return m_is_server; }

    // 允许WsServerImpl访问私有成员
    template<typename S>
    friend class WsServerImpl;

private:
    SocketType m_socket;
    RingBuffer m_ring_buffer;
    bool m_is_server;
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
