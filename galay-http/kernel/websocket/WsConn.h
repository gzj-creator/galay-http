#ifndef GALAY_WS_CONN_H
#define GALAY_WS_CONN_H

#include "WsReader.h"
#include "WsWriter.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/common/Buffer.h"

namespace galay::http {
    class HttpConn;   // 前向声明
}

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
     * @brief 从 HttpConn 构造 WebSocket 连接
     * @param http_conn HttpConn右值引用（将转移所有权）
     * @param reader_setting WsReaderSetting引用
     * @param writer_setting WsWriterSetting引用
     * @param is_server 是否是服务器端（默认true）
     *
     * @note 构造函数会自动设置 writer_setting.use_mask：
     *       - 客户端（is_server=false）：use_mask = true（协议要求）
     *       - 服务器端（is_server=true）：use_mask = false（协议要求）
     * @note 调用此构造函数后，http_conn 将处于无效状态，不应再使用
     */
    WsConn(galay::http::HttpConn&& http_conn,
           const WsReaderSetting& reader_setting,
           WsWriterSetting writer_setting,
           bool is_server = true);

    /**
     * @brief 构造函数（内部使用）
     * @param socket TcpSocket右值引用
     * @param ring_buffer RingBuffer右值引用
     * @param reader_setting WsReaderSetting引用
     * @param writer_setting WsWriterSetting引用
     * @param is_server 是否是服务器端（默认true）
     *
     * @note 构造函数会自动设置 writer_setting.use_mask：
     *       - 客户端（is_server=false）：use_mask = true（协议要求）
     *       - 服务器端（is_server=true）：use_mask = false（协议要求）
     */
    WsConn(TcpSocket&& socket,
           RingBuffer&& ring_buffer,
           const WsReaderSetting& reader_setting,
           WsWriterSetting writer_setting,  // 改为值传递，以便修改
           bool is_server = true)
        : m_socket(std::move(socket))
        , m_ring_buffer(std::move(ring_buffer))
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

    /**
     * @brief 析构函数
     */
    ~WsConn() = default;

    // 禁用拷贝
    WsConn(const WsConn&) = delete;
    WsConn& operator=(const WsConn&) = delete;

    // 禁用移动（因为 WsReader/WsWriter 包含引用成员，移动会导致引用失效）
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
     * @return WsReader& Reader对象引用
     */
    WsReader& getReader() {
        return m_reader;
    }

    /**
     * @brief 获取WsWriter
     * @return WsWriter& Writer对象引用
     */
    WsWriter& getWriter() {
        return m_writer;
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
    WsWriter m_writer;
    WsReader m_reader;
};

} // namespace galay::websocket

#endif // GALAY_WS_CONN_H
