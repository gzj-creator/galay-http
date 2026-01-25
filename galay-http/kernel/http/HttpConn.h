#ifndef GALAY_HTTP_CONN_H
#define GALAY_HTTP_CONN_H

#include "HttpReader.h"
#include "HttpWriter.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/common/Buffer.h"
#include <memory>
#include <optional>

namespace galay::websocket {
    class WsConn;  // 前向声明
}

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

    // 启用移动
    HttpConn(HttpConn&&) = default;
    HttpConn& operator=(HttpConn&&) = default;

    /**
     * @brief 关闭连接
     * @return CloseAwaitable 关闭等待体
     */
    CloseAwaitable close() {
        return m_socket.close();
    }

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

    // 允许HttpServer访问私有成员
    friend class HttpServer;

    // 允许HttpRouter访问私有成员
    friend class HttpRouter;

    // 允许WsConn访问私有成员（用于从HttpConn构造）
    friend class galay::websocket::WsConn;

private:
    /**
     * @brief 获取底层socket（私有方法，仅供友元类使用）
     * @return TcpSocket引用
     */
    TcpSocket& socket() { return m_socket; }

    /**
     * @brief 获取RingBuffer（私有方法，仅供友元类使用）
     * @return RingBuffer引用
     */
    RingBuffer& ringBuffer() { return m_ring_buffer; }

    TcpSocket m_socket;
    RingBuffer m_ring_buffer;
    HttpReaderSetting m_reader_setting;
    HttpWriterSetting m_writer_setting;
};

} // namespace galay::http

#endif // GALAY_HTTP_CONN_H
