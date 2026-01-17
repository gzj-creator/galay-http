#include "HttpServer.h"
#include "HttpLog.h"

namespace galay::http
{

HttpServer::HttpServer(const HttpServerConfig& config)
    : m_runtime(LoadBalanceStrategy::ROUND_ROBIN, config.io_scheduler_count, config.compute_scheduler_count)
    , m_config(config)
    , m_handler(nullptr)
    , m_listener(nullptr)
    , m_running(false)
{
}

HttpServer::~HttpServer()
{
    stop();
}

bool HttpServer::start()
{
    if (m_running.load()) {
        HTTP_LOG_WARN("server already running");
        return false;
    }

    if (!m_handler) {
        HTTP_LOG_ERROR("handler not set");
        return false;
    }

    // 启动 Runtime
    HTTP_LOG_INFO("starting runtime with {} IO schedulers and {} compute schedulers",
                  m_config.io_scheduler_count == 0 ? "auto" : std::to_string(m_config.io_scheduler_count),
                  m_config.compute_scheduler_count == 0 ? "auto" : std::to_string(m_config.compute_scheduler_count));

    m_runtime.start();

    HTTP_LOG_INFO("runtime started with {} IO schedulers and {} compute schedulers",
                  m_runtime.getIOSchedulerCount(),
                  m_runtime.getComputeSchedulerCount());

    // 获取第一个 IO 调度器用于监听
    auto* scheduler = m_runtime.getNextIOScheduler();
    if (!scheduler) {
        HTTP_LOG_ERROR("no IO scheduler available");
        m_runtime.stop();
        return false;
    }

    // 创建监听socket
    m_listener = std::make_unique<TcpSocket>(IPType::IPV4);

    // 设置socket选项
    auto reuse_result = m_listener->option().handleReuseAddr();
    if (!reuse_result) {
        HTTP_LOG_ERROR("failed to set reuse addr: {}", reuse_result.error().message());
        m_runtime.stop();
        return false;
    }

    auto nonblock_result = m_listener->option().handleNonBlock();
    if (!nonblock_result) {
        HTTP_LOG_ERROR("failed to set non-block: {}", nonblock_result.error().message());
        m_runtime.stop();
        return false;
    }

    // 绑定地址
    Host bind_host(IPType::IPV4, m_config.host, m_config.port);
    auto bind_result = m_listener->bind(bind_host);
    if (!bind_result) {
        HTTP_LOG_ERROR("failed to bind {}:{}: {}", m_config.host, m_config.port, bind_result.error().message());
        m_runtime.stop();
        return false;
    }

    // 开始监听
    auto listen_result = m_listener->listen(m_config.backlog);
    if (!listen_result) {
        HTTP_LOG_ERROR("failed to listen: {}", listen_result.error().message());
        m_runtime.stop();
        return false;
    }

    m_running.store(true);
    HTTP_LOG_INFO("HTTP server started on {}:{}", m_config.host, m_config.port);

    // 启动服务器循环协程
    scheduler->spawn(serverLoop());

    return true;
}

void HttpServer::stop()
{
    if (!m_running.load()) {
        return;
    }

    m_running.store(false);
    HTTP_LOG_INFO("HTTP server stopping...");

    // 关闭监听socket
    if (m_listener) {
        m_listener.reset();
    }

    // 停止 Runtime
    m_runtime.stop();

    HTTP_LOG_INFO("HTTP server stopped");
}

Coroutine HttpServer::serverLoop()
{
    while (m_running.load()) {
        // 接受新连接
        Host client_host;
        auto accept_result = co_await m_listener->accept(&client_host);

        if (!accept_result) {
            if (m_running.load()) {
                HTTP_LOG_ERROR("accept failed: {}", accept_result.error().message());
            }
            continue;
        }

        HTTP_LOG_INFO("client connected from {}:{}", client_host.ip(), client_host.port());

        // 使用负载均衡获取下一个 IO 调度器
        auto* scheduler = m_runtime.getNextIOScheduler();
        if (!scheduler) {
            HTTP_LOG_ERROR("no IO scheduler available");
            continue;
        }

        // 创建客户端socket并启动处理协程
        TcpSocket client_socket(accept_result.value());
        auto nonblock_result = client_socket.option().handleNonBlock();
        if (!nonblock_result) {
            HTTP_LOG_ERROR("failed to set client socket non-block: {}", nonblock_result.error().message());
            continue;
        }

        // 创建HttpConn
        HttpConn conn(std::move(client_socket), m_config.reader_setting, m_config.writer_setting);
        scheduler->spawn(m_handler(std::move(conn)));
    }

    co_return;
}

} // namespace galay::http
