#ifndef GALAY_WS_CONN_H
#define GALAY_WS_CONN_H

#include "WsReader.h"
#include "WsWriter.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/common/Buffer.h"

namespace galay::websocket
{

using namespace galay::async;
using namespace galay::kernel;

/**
 * @brief WebSocket连接类
 * @details 封装WebSocket连接的底层资源和配置
 */
class WsConn
{
public:
    /**
     * @brief 构造函数
     * @param socket TcpSocket右值引用
     * @param ring_buffer RingBuffer右值引用（从HttpConn转移）
     * @param reader_setting WsReaderSetting引用
     * @param writer_setting WsWriterSetting引用
     * @param is_server 是否是服务器端（默认true）
     */
    WsConn(TcpSocket&& socket,
           RingBuffer&& ring_buffer,
           const WsReaderSetting& reader_setting,
           const WsWriterSetting& writer_setting,
           bool is_server = true)
        : m_socket(std::move(socket))
        , m_ring_buffer(std::move(ring_buffer))
        , m_reader_setting(reader_setting)
        , m_writer_setting(writer_setting)
        , m_is_server(is_server)
    {
    }

    /**
     * @brief 析构函数
     */
    ~WsConn() = default;

    // 禁用拷贝
    WsConn(const WsConn&) = delete;
    WsConn& operator=(const WsConn&) = delete;

    // 禁用移动（因为WsReader/Writer包含引用成员）
    WsConn(WsConn&&) = delete;
    WsConn& operator=(WsConn&&) = delete;

    /**
     * @brief 关闭连接
     * @return CloseAwaitable 关闭等待体
     */
    CloseAwaitable close() {
        return m_socket.close();
    }

    /**
     * @brief 获取底层socket
     * @return TcpSocket引用
     */
    TcpSocket& socket() { return m_socket; }

    /**
     * @brief 获取RingBuffer
     * @return RingBuffer引用
     */
    RingBuffer& ringBuffer() { return m_ring_buffer; }

    /**
     * @brief 获取WsReader
     * @return WsReader 临时构造的Reader对象
     */
    WsReader getReader() {
        return WsReader(m_ring_buffer, m_reader_setting, m_socket, m_is_server);
    }

    /**
     * @brief 获取WsWriter
     * @return WsWriter 临时构造的Writer对象
     */
    WsWriter getWriter() {
        return WsWriter(m_writer_setting, m_socket);
    }

    /**
     * @brief 检查是否是服务器端
     */
    bool isServer() const { return m_is_server; }

private:
    TcpSocket m_socket;
    RingBuffer m_ring_buffer;
    WsReaderSetting m_reader_setting;
    WsWriterSetting m_writer_setting;
    bool m_is_server;
};

} // namespace galay::websocket

#endif // GALAY_WS_CONN_H
