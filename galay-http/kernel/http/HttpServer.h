#ifndef GALAY_HTTP_SERVER_H
#define GALAY_HTTP_SERVER_H

#include "HttpConn.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/kernel/IOScheduler.hpp"
#include "galay-kernel/kernel/Coroutine.h"
#include <memory>
#include <atomic>
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
 * @brief HTTP服务器配置
 */
struct HttpServerConfig
{
    std::string host = "0.0.0.0";
    uint16_t port = 8080;
    int backlog = 128;
    HttpReaderSetting reader_setting;
    HttpWriterSetting writer_setting;
};

/**
 * @brief HTTP服务器类
 * @details 提供HTTP服务器功能，监听端口并处理HTTP请求
 */
class HttpServer
{
public:
    /**
     * @brief 构造函数
     * @param scheduler IO调度器指针
     * @param config 服务器配置
     */
    HttpServer(IOScheduler* scheduler, const HttpServerConfig& config);

    /**
     * @brief 析构函数
     */
    ~HttpServer();

    // 禁用拷贝
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    /**
     * @brief 设置HTTP请求处理器
     * @param handler 处理函数
     */
    void setHandler(HttpHandler handler) {
        m_handler = std::move(handler);
    }

    /**
     * @brief 启动服务器
     * @return 是否启动成功
     */
    bool start();

    /**
     * @brief 停止服务器
     */
    void stop();

    /**
     * @brief 检查服务器是否正在运行
     * @return 是否正在运行
     */
    bool isRunning() const {
        return m_running.load();
    }

private:
    /**
     * @brief 服务器主循环协程
     */
    Coroutine serverLoop();

    /**
     * @brief 处理单个连接的协程
     * @param socket 客户端socket
     */
    Coroutine handleConnection(TcpSocket socket);

private:
    IOScheduler* m_scheduler;
    HttpServerConfig m_config;
    HttpHandler m_handler;
    std::unique_ptr<TcpSocket> m_listener;
    std::atomic<bool> m_running;
};

} // namespace galay::http

#endif // GALAY_HTTP_SERVER_H
