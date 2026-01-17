#ifndef GALAY_HTTP_CONN_H
#define GALAY_HTTP_CONN_H

#include "HttpReader.h"
#include "HttpWriter.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/common/Buffer.h"
#include <memory>
#include <optional>

namespace galay::http
{

using namespace galay::async;
using namespace galay::kernel;

/**
 * @brief HTTP连接类
 * @details 封装HTTP连接的底层资源和配置，不处理业务逻辑
 */
class HttpConn
{
public:
    /**
     * @brief 构造函数
     * @param socket TcpSocket右值引用
     * @param reader_setting HttpReaderSetting引用
     * @param writer_setting HttpWriterSetting引用
     */
    HttpConn(TcpSocket&& socket, const HttpReaderSetting& reader_setting, const HttpWriterSetting& writer_setting);

    /**
     * @brief 析构函数
     */
    ~HttpConn() = default;

    // 禁用拷贝
    HttpConn(const HttpConn&) = delete;
    HttpConn& operator=(const HttpConn&) = delete;

    // 禁用移动（因为HttpReader/Writer包含引用成员）
    HttpConn(HttpConn&&) = delete;
    HttpConn& operator=(HttpConn&&) = delete;

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
     * @brief 获取HttpReader
     * @return HttpReader 临时构造的Reader对象
     */
    HttpReader getReader() {
        return HttpReader(m_ring_buffer, m_reader_setting, m_socket);
    }

    /**
     * @brief 获取HttpWriter
     * @return HttpWriter 临时构造的Writer对象
     */
    HttpWriter getWriter() {
        return HttpWriter(m_writer_setting, m_socket);
    }

    /**
     * @brief 升级到 WebSocket 连接
     * @tparam WsConnType WebSocket连接类型（通常是 galay::websocket::WsConn）
     * @param ws_reader_setting WebSocket读取器配置
     * @param ws_writer_setting WebSocket写入器配置
     * @return std::unique_ptr<WsConnType> WebSocket连接的智能指针
     * @details 此方法会转移 socket 和 ring_buffer 的所有权到新的 WebSocket 连接
     *          调用此方法后，HttpConn 对象将处于无效状态，不应再使用
     */
    template<typename WsConnType, typename WsReaderSetting, typename WsWriterSetting>
    std::unique_ptr<WsConnType> upgrade(const WsReaderSetting& ws_reader_setting,
                                       const WsWriterSetting& ws_writer_setting,
                                       bool is_server = true) {
        return std::make_unique<WsConnType>(
            std::move(m_socket),
            std::move(m_ring_buffer),
            ws_reader_setting,
            ws_writer_setting,
            is_server
        );
    }

    // 允许HttpServer访问私有成员
    friend class HttpServer;

private:
    TcpSocket m_socket;
    RingBuffer m_ring_buffer;
    HttpReaderSetting m_reader_setting;
    HttpWriterSetting m_writer_setting;
};

} // namespace galay::http

#endif // GALAY_HTTP_CONN_H
