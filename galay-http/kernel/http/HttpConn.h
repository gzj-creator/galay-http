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

namespace galay::http2 {
    template<typename SocketType>
    class Http2ConnImpl;  // 前向声明
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
     */
    HttpConnImpl(SocketType&& socket)
        : m_socket(std::move(socket))
        , m_ring_buffer(8192)  // 8KB buffer
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
     * @brief SSL shutdown（仅对 SslSocket 有效）
     * @return ShutdownAwaitable shutdown等待体
     */
    auto shutdown() {
        return m_socket.shutdown();
    }

    /**
     * @brief 获取HttpReader
     * @param setting HttpReaderSetting配置
     * @return HttpReaderImpl<SocketType> Reader对象
     */
    HttpReaderImpl<SocketType> getReader(const HttpReaderSetting& setting = HttpReaderSetting()) {
        return HttpReaderImpl<SocketType>(m_ring_buffer, setting, m_socket);
    }

    /**
     * @brief 获取HttpWriter
     * @param setting HttpWriterSetting配置
     * @return HttpWriterImpl<SocketType> Writer对象
     */
    HttpWriterImpl<SocketType> getWriter(const HttpWriterSetting& setting = HttpWriterSetting()) {
        return HttpWriterImpl<SocketType>(setting, m_socket);
    }

    /**
     * @brief 获取底层 Socket 引用
     * @return SocketType 引用
     * @note 用于需要直接访问底层 socket 的场景（如 WebSocket 升级后的处理）
     */
    SocketType& getSocket() { return m_socket; }

    // 允许HttpServerImpl访问私有成员
    template<typename S>
    friend class HttpServerImpl;

    // 允许HttpRouterImpl访问私有成员
    friend class HttpRouter;

    // 允许WsConnImpl访问私有成员（用于从HttpConnImpl构造）
    template<typename S>
    friend class galay::websocket::WsConnImpl;

    // 允许Http2ConnImpl访问私有成员（用于h2c升级）
    template<typename S>
    friend class galay::http2::Http2ConnImpl;

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
