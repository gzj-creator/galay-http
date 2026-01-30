#ifndef GALAY_HTTP_CONN_H
#define GALAY_HTTP_CONN_H

#include "HttpReader.h"
#include "HttpWriter.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/common/Buffer.h"

namespace galay::websocket {
    template<typename SocketType>
    class WsConnImpl;  // 前向声明
}

namespace galay::http
{

using namespace galay::async;
using namespace galay::kernel;

/**
 * @brief HTTP连接模板类
 * @tparam SocketType Socket类型（TcpSocket 或 SslSocket）
 * @details 封装HTTP连接的底层资源和配置，不处理业务逻辑
 */
template<typename SocketType>
class HttpConnImpl
{
public:
    /**
     * @brief 构造函数
     * @param socket Socket右值引用
     * @param reader_setting HttpReaderSetting引用
     * @param writer_setting HttpWriterSetting引用
     */
    HttpConnImpl(SocketType&& socket, const HttpReaderSetting& reader_setting, const HttpWriterSetting& writer_setting)
        : m_socket(std::move(socket))
        , m_ring_buffer(8192)  // 8KB buffer
        , m_reader_setting(reader_setting)
        , m_writer_setting(writer_setting)
    {
    }

    ~HttpConnImpl() = default;

    // 禁用拷贝
    HttpConnImpl(const HttpConnImpl&) = delete;
    HttpConnImpl& operator=(const HttpConnImpl&) = delete;

    // 启用移动
    HttpConnImpl(HttpConnImpl&&) = default;
    HttpConnImpl& operator=(HttpConnImpl&&) = default;

    /**
     * @brief 关闭连接
     * @return CloseAwaitable 关闭等待体
     */
    auto close() {
        return m_socket.close();
    }

    /**
     * @brief 获取HttpReader
     * @return HttpReaderImpl<SocketType> 临时构造的Reader对象
     */
    HttpReaderImpl<SocketType> getReader() {
        return HttpReaderImpl<SocketType>(m_ring_buffer, m_reader_setting, m_socket);
    }

    /**
     * @brief 获取HttpWriter
     * @return HttpWriterImpl<SocketType> 临时构造的Writer对象
     */
    HttpWriterImpl<SocketType> getWriter() {
        return HttpWriterImpl<SocketType>(m_writer_setting, m_socket);
    }

    // 允许HttpServerImpl访问私有成员
    template<typename S>
    friend class HttpServerImpl;

    // 允许HttpRouterImpl访问私有成员
    friend class HttpRouter;

    // 允许WsConnImpl访问私有成员（用于从HttpConnImpl构造）
    template<typename S>
    friend class galay::websocket::WsConnImpl;

private:
    /**
     * @brief 获取底层socket（私有方法，仅供友元类使用）
     * @return SocketType引用
     */
    SocketType& socket() { return m_socket; }

    /**
     * @brief 获取RingBuffer（私有方法，仅供友元类使用）
     * @return RingBuffer引用
     */
    RingBuffer& ringBuffer() { return m_ring_buffer; }

    SocketType m_socket;
    RingBuffer m_ring_buffer;
    HttpReaderSetting m_reader_setting;
    HttpWriterSetting m_writer_setting;
};

// 类型别名 - HTTP (TcpSocket)
using HttpConn = HttpConnImpl<TcpSocket>;

} // namespace galay::http

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/SslSocket.h"
namespace galay::http {
using HttpsConn = HttpConnImpl<galay::ssl::SslSocket>;
} // namespace galay::http
#endif

#endif // GALAY_HTTP_CONN_H
