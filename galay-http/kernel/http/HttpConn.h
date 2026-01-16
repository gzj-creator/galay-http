#ifndef GALAY_HTTP_CONN_H
#define GALAY_HTTP_CONN_H

#include "HttpReader.h"
#include "HttpWriter.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/common/Buffer.h"
#include "galay-kernel/kernel/Coroutine.h"
#include <memory>
#include <functional>

namespace galay::http
{

using namespace galay::async;
using namespace galay::kernel;

/**
 * @brief HTTP连接处理器类型
 * @details 用户提供的处理函数，接收HttpRequest并返回HttpResponse
 */
using HttpHandler = std::function<void(HttpRequest&, HttpResponse&)>;

/**
 * @brief HTTP连接类
 * @details 封装单个HTTP连接的读写操作
 */
class HttpConn
{
public:
    /**
     * @brief 构造函数
     * @param socket TcpSocket右值引用
     * @param setting HttpReaderSetting引用
     */
    HttpConn(TcpSocket&& socket, const HttpReaderSetting& setting);

    /**
     * @brief 析构函数
     */
    ~HttpConn() = default;

    // 禁用拷贝
    HttpConn(const HttpConn&) = delete;
    HttpConn& operator=(const HttpConn&) = delete;

    // 禁用移动（因为HttpReader包含引用成员）
    HttpConn(HttpConn&&) = delete;
    HttpConn& operator=(HttpConn&&) = delete;

    /**
     * @brief 处理HTTP连接
     * @param handler 用户提供的处理函数
     * @return Coroutine 协程对象
     */
    Coroutine handle(HttpHandler handler);

    /**
     * @brief 关闭连接
     * @return Coroutine 协程对象
     */
    Coroutine close();

    /**
     * @brief 获取底层socket
     * @return TcpSocket引用
     */
    TcpSocket& socket() { return m_socket; }

    // 允许HttpServer访问私有成员
    friend class HttpServer;

private:
    TcpSocket m_socket;
    RingBuffer m_ring_buffer;
    HttpReaderSetting m_setting;
    HttpReader m_reader;
    HttpWriter m_writer;
};

} // namespace galay::http

#endif // GALAY_HTTP_CONN_H
