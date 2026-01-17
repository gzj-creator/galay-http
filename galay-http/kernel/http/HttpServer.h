#ifndef GALAY_HTTP_SERVER_H
#define GALAY_HTTP_SERVER_H

#include "HttpConn.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/kernel/Runtime.h"
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
 * @details 用户提供的处理函数，接收HttpConn引用
 */
using HttpHandler = std::function<Coroutine(HttpConn)>;

/**
 * @brief HTTP服务器配置
 */
struct HttpServerConfig
{
    std::string host = "0.0.0.0";
    uint16_t port = 8080;
    int backlog = 128;
    size_t io_scheduler_count = 0;      // 0 表示自动（2 * CPU 核心数）
    size_t compute_scheduler_count = 0; // 0 表示自动（CPU 核心数）
    HttpReaderSetting reader_setting;
    HttpWriterSetting writer_setting;
};

/**
 * @brief HTTP服务器类
 * @details 提供HTTP服务器功能，内置 Runtime 管理调度器
 */
class HttpServer
{
public:
    /**
     * @brief 构造函数
     * @param config 服务器配置
     */
    explicit HttpServer(const HttpServerConfig& config = HttpServerConfig());

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
     * @details 会自动启动内置的 Runtime
     */
    bool start();

    /**
     * @brief 停止服务器
     * @details 会自动停止内置的 Runtime
     */
    void stop();

    /**
     * @brief 检查服务器是否正在运行
     * @return 是否正在运行
     */
    bool isRunning() const {
        return m_running.load();
    }

    /**
     * @brief 获取 Runtime 引用
     * @return Runtime 引用
     */
    Runtime& getRuntime() {
        return m_runtime;
    }

private:
    /**
     * @brief 服务器主循环协程
     */
    Coroutine serverLoop();

private:
    Runtime m_runtime;                  // 内置的 Runtime
    HttpServerConfig m_config;
    HttpHandler m_handler;
    std::unique_ptr<TcpSocket> m_listener;
    std::atomic<bool> m_running;
};

} // namespace galay::http

#endif // GALAY_HTTP_SERVER_H
