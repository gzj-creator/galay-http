#ifndef GALAY_HTTP_SERVER_H
#define GALAY_HTTP_SERVER_H

#include "HttpConn.h"
#include "HttpRouter.h"
#include "HttpLog.h"
#include "galay-http/utils/Http1_1ResponseBuilder.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/kernel/Runtime.h"
#include <memory>
#include <atomic>
#include <functional>
#include <cstdint>
#include <optional>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/async/SslSocket.h"
#include "galay-ssl/ssl/SslContext.h"
#endif

namespace galay::http
{

using namespace galay::async;
using namespace galay::kernel;

// 前向声明
template<typename SocketType>
class HttpServerImpl;

/**
 * @brief HTTP连接处理器类型
 */
template<typename SocketType>
using HttpConnHandlerImpl = std::function<Task<void>(HttpConnImpl<SocketType>)>;

/**
 * @brief HTTP服务器配置
 */
struct HttpServerConfig
{
    std::string host = "0.0.0.0";
    uint16_t port = 8080;
    int backlog = 128;
    size_t io_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO;
    size_t compute_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO;
    RuntimeAffinityConfig affinity;
};

class HttpServerBuilder {
public:
    HttpServerBuilder& host(std::string v)              { m_config.host = std::move(v); return *this; }
    HttpServerBuilder& port(uint16_t v)                 { m_config.port = v; return *this; }
    HttpServerBuilder& backlog(int v)                   { m_config.backlog = v; return *this; }
    HttpServerBuilder& ioSchedulerCount(size_t v)       { m_config.io_scheduler_count = v; return *this; }
    HttpServerBuilder& computeSchedulerCount(size_t v)  { m_config.compute_scheduler_count = v; return *this; }
    HttpServerBuilder& sequentialAffinity(size_t io_count, size_t compute_count) {
        m_config.affinity.mode = RuntimeAffinityConfig::Mode::Sequential;
        m_config.affinity.seq_io_count = io_count;
        m_config.affinity.seq_compute_count = compute_count;
        return *this;
    }
    bool customAffinity(std::vector<uint32_t> io_cpus, std::vector<uint32_t> compute_cpus) {
        if (io_cpus.size() != m_config.io_scheduler_count ||
            compute_cpus.size() != m_config.compute_scheduler_count) {
            return false;
        }
        m_config.affinity.mode = RuntimeAffinityConfig::Mode::Custom;
        m_config.affinity.custom_io_cpus = std::move(io_cpus);
        m_config.affinity.custom_compute_cpus = std::move(compute_cpus);
        return true;
    }
    HttpServerImpl<TcpSocket> build() const;
    HttpServerConfig buildConfig() const                { return m_config; }
private:
    HttpServerConfig m_config;
};

/**
 * @brief HTTP服务器模板类
 */
template<typename SocketType>
class HttpServerImpl
{
public:
    using ConnHandler = HttpConnHandlerImpl<SocketType>;

    explicit HttpServerImpl(const HttpServerConfig& config = HttpServerConfig())
        : m_runtime(RuntimeBuilder().ioSchedulerCount(config.io_scheduler_count)
                                   .computeSchedulerCount(config.compute_scheduler_count)
                                   .applyAffinity(config.affinity)
                                   .build())
        , m_config(config)
        , m_handler(nullptr)
        , m_listener(nullptr)
        , m_running(false)
    {
    }

    virtual ~HttpServerImpl() {
        stop();
    }

    HttpServerImpl(const HttpServerImpl&) = delete;
    HttpServerImpl& operator=(const HttpServerImpl&) = delete;

    void start(ConnHandler handler) {
        m_handler = handler;
        startInternal();
    }

    void start(HttpRouter&& router) {
        m_router = std::move(router);

        m_handler = [this](HttpConnImpl<SocketType> conn) -> Task<void> {
            HTTP_LOG_DEBUG("[handler] [start]");
            bool keep_alive = true;

            while (keep_alive) {
                HTTP_LOG_DEBUG("[reader] [create]");
                auto reader = conn.getReader();
                HTTP_LOG_DEBUG("[reader] [ready]");
                HttpRequest request;
                auto read_result = co_await reader.getRequest(request);

                if (!read_result) {
                    const auto& error = read_result.error();
                    const std::string message = error.message();
                    const bool is_disconnect_like =
                        error.code() == kConnectionClose ||
                        ((error.code() == kRecvError || error.code() == kTcpRecvError) &&
                         message.find("Connection disconnected") != std::string::npos);

                    if (is_disconnect_like) {
                        HTTP_LOG_WARN("[disconnect] [{}]", message);
                    } else if (error.code() == kRecvTimeOut || error.code() == kSendTimeOut || error.code() == kRequestTimeOut) {
                        HTTP_LOG_WARN("[timeout] [request] [{}]", message);
                    } else {
                        HTTP_LOG_ERROR("[recv] [fail] [{}]", message);
                    }
                    break;
                }

                HTTP_LOG_DEBUG("[req] [read-ok]");
                keep_alive = request.header().isKeepAlive() && !request.header().isConnectionClose();

                auto match = m_router->findHandler(request.header().method(), request.header().uri());

                if (!match.handler && m_router->hasFallbackProxy()) {
                    match.handler = m_router->fallbackProxyHandler();
                    HTTP_LOG_DEBUG("[proxy-fallback] [hit] [{}] [{}]",
                                   httpMethodToString(request.header().method()),
                                   request.header().uri());
                }

                if (!match.handler) {
                    HTTP_LOG_WARN("[route] [miss] [{}] [{}]",
                                 httpMethodToString(request.header().method()),
                                 request.header().uri());

                    auto response = Http1_1ResponseBuilder()
                        .status(HttpStatusCode::OK_200)
                        .header("Content-Type", "text/plain")
                        .body("404 Not Found")
                        .buildMove();

                    auto writer = conn.getWriter();
                    auto result = co_await writer.sendResponse(response);
                    if (!result) {
                        HTTP_LOG_ERROR("[send] [fail] [{}]", result.error().message());
                    }

                    if (!keep_alive) {
                        break;
                    }
                    continue;
                }

                HTTP_LOG_DEBUG("[handler] [call]");
                if constexpr (std::is_same_v<SocketType, TcpSocket>) {
                    co_await (*match.handler)(conn, std::move(request));
                } else {
                    HTTP_LOG_ERROR("[router] [https] [unsupported]");
                    break;
                }
                HTTP_LOG_DEBUG("[handler] [done]");

                if (!keep_alive) {
                    break;
                }
            }

            co_await conn.close();
            co_return;
        };

        startInternal();
    }

    void stop() {
        if (!m_running.load()) {
            return;
        }

        m_running.store(false);
        HTTP_LOG_INFO("[server] [stopping]");

        if (m_listener) {
            m_listener.reset();
        }

        m_runtime.stop();

        HTTP_LOG_INFO("[server] [stopped]");
    }

    bool isRunning() const {
        return m_running.load();
    }

    Runtime& getRuntime() {
        return m_runtime;
    }

protected:
    virtual bool startInternal() {
        if (m_running.load()) {
            HTTP_LOG_WARN("[server] [already-running]");
            return false;
        }

        if (!m_handler) {
            HTTP_LOG_ERROR("[server] [handler-missing]");
            return false;
        }

        HTTP_LOG_INFO("[runtime] [start] [io={}] [compute={}]",
                      m_config.io_scheduler_count == GALAY_RUNTIME_SCHEDULER_COUNT_AUTO ? "auto" : std::to_string(m_config.io_scheduler_count),
                      m_config.compute_scheduler_count == GALAY_RUNTIME_SCHEDULER_COUNT_AUTO ? "auto" : std::to_string(m_config.compute_scheduler_count));

        m_runtime.start();

        HTTP_LOG_INFO("[runtime] [started] [io={}] [compute={}]",
                      m_runtime.getIOSchedulerCount(),
                      m_runtime.getComputeSchedulerCount());

        m_running.store(true);
        HTTP_LOG_INFO("[server] [listen] [{}:{}]", m_config.host, m_config.port);

        // 在每个 IO 调度器上启动一个 serverLoop，每个 serverLoop 创建自己的 listener
        // 利用 SO_REUSEPORT 实现多线程 accept
        size_t io_scheduler_count = m_runtime.getIOSchedulerCount();
        for (size_t i = 0; i < io_scheduler_count; i++) {
            auto* scheduler = m_runtime.getIOScheduler(i);
            if (scheduler) {
                scheduleTask(scheduler, serverLoop(scheduler));
            }
        }

        return true;
    }

    virtual Task<void> serverLoop(IOScheduler* scheduler) {
        // 每个 serverLoop 创建自己的 listener socket
        TcpSocket listener(IPType::IPV4);

        HTTP_LOG_DEBUG("[loop] [start]");

        auto reuse_result = listener.option().handleReuseAddr();
        if (!reuse_result) {
            HTTP_LOG_ERROR("[socket] [reuseaddr-fail] [{}]", reuse_result.error().message());
            co_return;
        }

        // 设置 SO_REUSEPORT 以支持多线程 accept
        auto reuse_port_result = listener.option().handleReusePort();
        if (!reuse_port_result) {
            HTTP_LOG_ERROR("[socket] [reuseport-fail] [{}]", reuse_port_result.error().message());
            co_return;
        }

        auto nonblock_result = listener.option().handleNonBlock();
        if (!nonblock_result) {
            HTTP_LOG_ERROR("[socket] [nonblock-fail] [{}]", nonblock_result.error().message());
            co_return;
        }

        Host bind_host(IPType::IPV4, m_config.host, m_config.port);
        auto bind_result = listener.bind(bind_host);
        if (!bind_result) {
            HTTP_LOG_ERROR("[bind] [fail] [{}:{}] [{}]", m_config.host, m_config.port, bind_result.error().message());
            co_return;
        }

        auto listen_result = listener.listen(m_config.backlog);
        if (!listen_result) {
            HTTP_LOG_ERROR("[listen] [fail] [{}]", listen_result.error().message());
            co_return;
        }

        HTTP_LOG_DEBUG("[loop] [ready]");

        while (m_running.load()) {
            Host client_host;
            auto accept_result = co_await listener.accept(&client_host);

            if (!accept_result) {
                if (m_running.load()) {
                    HTTP_LOG_ERROR("[accept] [fail] [{}]", accept_result.error().message());
                }
                continue;
            }

            HTTP_LOG_INFO("[connect] [{}:{}]", client_host.ip(), client_host.port());

            auto client_socket_opt = createClientSocket(accept_result.value());
            if (!client_socket_opt) {
                HTTP_LOG_ERROR("[socket] [create-fail]");
                continue;
            }

            HTTP_LOG_DEBUG("[socket] [nonblock]");

            SocketType client_socket = std::move(*client_socket_opt);
            auto nonblock_result = client_socket.option().handleNonBlock();
            if (!nonblock_result) {
                HTTP_LOG_ERROR("[socket] [nonblock-fail] [{}]", nonblock_result.error().message());
                continue;
            }

            HTTP_LOG_DEBUG("[conn] [create]");
            HttpConnImpl<SocketType> conn(std::move(client_socket));
            HTTP_LOG_DEBUG("[handler] [spawn]");

            // 在当前调度器上处理连接
            scheduleTask(scheduler, m_handler(std::move(conn)));
            HTTP_LOG_DEBUG("[handler] [spawned]");
        }

        co_return;
    }

    virtual std::optional<SocketType> createClientSocket(GHandle fd) {
        if constexpr (std::is_same_v<SocketType, TcpSocket>) {
            return SocketType(fd);
        } else {
            // SslSocket 需要在派生类中实现
            return std::nullopt;
        }
    }

protected:
    Runtime m_runtime;
    HttpServerConfig m_config;
    ConnHandler m_handler;
    std::optional<HttpRouter> m_router;
    std::unique_ptr<TcpSocket> m_listener;
    std::atomic<bool> m_running;
};

// 类型别名 - HTTP (TcpSocket)
using HttpConnHandler = HttpConnHandlerImpl<TcpSocket>;
using HttpServer = HttpServerImpl<TcpSocket>;
inline HttpServer HttpServerBuilder::build() const { return HttpServer(m_config); }

#ifdef GALAY_HTTP_SSL_ENABLED
/**
 * @brief HTTPS 服务器配置
 */
struct HttpsServerConfig
{
    std::string host = "0.0.0.0";
    uint16_t port = 443;
    int backlog = 128;
    size_t io_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO;
    size_t compute_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO;
    RuntimeAffinityConfig affinity;
    HttpReaderSetting reader_setting;
    HttpWriterSetting writer_setting;
    std::string cert_path;
    std::string key_path;
    std::string ca_path;
    bool verify_peer = false;
    int verify_depth = 4;
};

class HttpsServer;

class HttpsServerBuilder {
public:
    HttpsServerBuilder& host(std::string v)              { m_config.host = std::move(v); return *this; }
    HttpsServerBuilder& port(uint16_t v)                 { m_config.port = v; return *this; }
    HttpsServerBuilder& backlog(int v)                   { m_config.backlog = v; return *this; }
    HttpsServerBuilder& ioSchedulerCount(size_t v)       { m_config.io_scheduler_count = v; return *this; }
    HttpsServerBuilder& computeSchedulerCount(size_t v)  { m_config.compute_scheduler_count = v; return *this; }
    HttpsServerBuilder& sequentialAffinity(size_t io_count, size_t compute_count) {
        m_config.affinity.mode = RuntimeAffinityConfig::Mode::Sequential;
        m_config.affinity.seq_io_count = io_count;
        m_config.affinity.seq_compute_count = compute_count;
        return *this;
    }
    bool customAffinity(std::vector<uint32_t> io_cpus, std::vector<uint32_t> compute_cpus) {
        if (io_cpus.size() != m_config.io_scheduler_count ||
            compute_cpus.size() != m_config.compute_scheduler_count) {
            return false;
        }
        m_config.affinity.mode = RuntimeAffinityConfig::Mode::Custom;
        m_config.affinity.custom_io_cpus = std::move(io_cpus);
        m_config.affinity.custom_compute_cpus = std::move(compute_cpus);
        return true;
    }
    HttpsServerBuilder& certPath(std::string v)          { m_config.cert_path = std::move(v); return *this; }
    HttpsServerBuilder& keyPath(std::string v)           { m_config.key_path = std::move(v); return *this; }
    HttpsServerBuilder& caPath(std::string v)            { m_config.ca_path = std::move(v); return *this; }
    HttpsServerBuilder& verifyPeer(bool v)               { m_config.verify_peer = v; return *this; }
    HttpsServerBuilder& verifyDepth(int v)               { m_config.verify_depth = v; return *this; }
    HttpsServer build() const;
    HttpsServerConfig buildConfig() const                { return m_config; }
private:
    HttpsServerConfig m_config;
};

using HttpsConnHandler = HttpConnHandlerImpl<galay::ssl::SslSocket>;

/**
 * @brief HTTPS服务器类
 */
class HttpsServer : public HttpServerImpl<galay::ssl::SslSocket>
{
public:
    explicit HttpsServer(const HttpsServerConfig& config)
        : HttpServerImpl<galay::ssl::SslSocket>(convertConfig(config))
        , m_https_config(config)
        , m_ssl_ctx(galay::ssl::SslMethod::TLS_Server)
    {
    }

    ~HttpsServer() override = default;

protected:
    bool startInternal() override {
        // 初始化 SSL 上下文
        if (!initSslContext()) {
            HTTP_LOG_ERROR("[ssl] [context] [init-fail]");
            return false;
        }

        return HttpServerImpl<galay::ssl::SslSocket>::startInternal();
    }

    std::optional<galay::ssl::SslSocket> createClientSocket(GHandle fd) override {
        if (!m_ssl_ctx.isValid()) {
            HTTP_LOG_ERROR("[ssl] [context] [missing]");
            return std::nullopt;
        }

        return galay::ssl::SslSocket(&m_ssl_ctx, fd);
    }

    Task<void> serverLoop(IOScheduler* scheduler) override {
        // 每个 serverLoop 创建自己的 listener socket
        TcpSocket listener(IPType::IPV4);

        auto reuse_result = listener.option().handleReuseAddr();
        if (!reuse_result) {
            HTTP_LOG_ERROR("[socket] [reuseaddr-fail] [{}]", reuse_result.error().message());
            co_return;
        }

        // 设置 SO_REUSEPORT 以支持多线程 accept
        auto reuse_port_result = listener.option().handleReusePort();
        if (!reuse_port_result) {
            HTTP_LOG_ERROR("[socket] [reuseport-fail] [{}]", reuse_port_result.error().message());
            co_return;
        }

        auto nonblock_result = listener.option().handleNonBlock();
        if (!nonblock_result) {
            HTTP_LOG_ERROR("[socket] [nonblock-fail] [{}]", nonblock_result.error().message());
            co_return;
        }

        Host bind_host(IPType::IPV4, m_config.host, m_config.port);
        auto bind_result = listener.bind(bind_host);
        if (!bind_result) {
            HTTP_LOG_ERROR("[bind] [fail] [{}:{}] [{}]", m_config.host, m_config.port, bind_result.error().message());
            co_return;
        }

        auto listen_result = listener.listen(m_config.backlog);
        if (!listen_result) {
            HTTP_LOG_ERROR("[listen] [fail] [{}]", listen_result.error().message());
            co_return;
        }

        while (m_running.load()) {
            Host client_host;
            auto accept_result = co_await listener.accept(&client_host);

            if (!accept_result) {
                if (m_running.load()) {
                    HTTP_LOG_ERROR("[accept] [fail] [{}]", accept_result.error().message());
                }
                continue;
            }

            HTTP_LOG_INFO("[connect] [https] [{}:{}]", client_host.ip(), client_host.port());

            auto client_socket_opt = createClientSocket(accept_result.value());
            if (!client_socket_opt) {
                HTTP_LOG_ERROR("[socket] [create-fail] [ssl]");
                continue;
            }

            galay::ssl::SslSocket client_socket = std::move(*client_socket_opt);
            auto nonblock_result = client_socket.option().handleNonBlock();
            if (!nonblock_result) {
                HTTP_LOG_ERROR("[socket] [nonblock-fail] [{}]", nonblock_result.error().message());
                continue;
            }

            // 在当前调度器上执行 SSL 握手和处理
            scheduleTask(scheduler, handleSslConnection(std::move(client_socket)));
        }

        co_return;
    }

private:
    Task<void> handleSslConnection(galay::ssl::SslSocket socket) {
        auto handshake_result = co_await socket.handshake();
        if (!handshake_result) {
            HTTP_LOG_ERROR("[ssl] [handshake-fail] [{}]", handshake_result.error().message());
            co_await socket.close();
            co_return;
        }

        HTTP_LOG_DEBUG("[ssl] [handshake-ok]");

        // 创建连接并调用处理器
        HttpConnImpl<galay::ssl::SslSocket> conn(std::move(socket));
        co_await m_handler(std::move(conn));
        co_return;
    }

    static HttpServerConfig convertConfig(const HttpsServerConfig& config) {
        HttpServerConfig base_config;
        base_config.host = config.host;
        base_config.port = config.port;
        base_config.backlog = config.backlog;
        base_config.io_scheduler_count = config.io_scheduler_count;
        base_config.compute_scheduler_count = config.compute_scheduler_count;
        base_config.affinity = config.affinity;
        return base_config;
    }

    bool initSslContext() {
        if (!m_ssl_ctx.isValid()) {
            HTTP_LOG_ERROR("[ssl] [context] [create-fail]");
            return false;
        }

        // 加载证书
        if (!m_https_config.cert_path.empty()) {
            auto result = m_ssl_ctx.loadCertificate(m_https_config.cert_path);
            if (!result) {
                HTTP_LOG_ERROR("[ssl] [cert] [load-fail] [{}] [{}]",
                              m_https_config.cert_path, result.error().message());
                return false;
            }
            HTTP_LOG_INFO("[ssl] [cert] [{}]", m_https_config.cert_path);
        }

        // 加载私钥
        if (!m_https_config.key_path.empty()) {
            auto result = m_ssl_ctx.loadPrivateKey(m_https_config.key_path);
            if (!result) {
                HTTP_LOG_ERROR("[ssl] [key] [load-fail] [{}] [{}]",
                              m_https_config.key_path, result.error().message());
                return false;
            }
            HTTP_LOG_INFO("[ssl] [key] [{}]", m_https_config.key_path);
        }

        // 加载 CA 证书
        if (!m_https_config.ca_path.empty()) {
            auto result = m_ssl_ctx.loadCACertificate(m_https_config.ca_path);
            if (!result) {
                HTTP_LOG_ERROR("[ssl] [ca] [load-fail] [{}]", m_https_config.ca_path);
                return false;
            }
            HTTP_LOG_INFO("[ssl] [ca] [{}]", m_https_config.ca_path);
        }

        // 设置验证模式
        if (m_https_config.verify_peer) {
            m_ssl_ctx.setVerifyMode(galay::ssl::SslVerifyMode::Peer);
            m_ssl_ctx.setVerifyDepth(m_https_config.verify_depth);
            HTTP_LOG_INFO("[ssl] [verify-client] [enabled]");
        } else {
            m_ssl_ctx.setVerifyMode(galay::ssl::SslVerifyMode::None);
        }

        HTTP_LOG_INFO("[ssl] [context] [ready]");
        return true;
    }

    HttpsServerConfig m_https_config;
    galay::ssl::SslContext m_ssl_ctx;
};

inline HttpsServer HttpsServerBuilder::build() const { return HttpsServer(m_config); }
#endif

} // namespace galay::http

#endif // GALAY_HTTP_SERVER_H
