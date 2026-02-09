#ifndef GALAY_HTTP_SERVER_H
#define GALAY_HTTP_SERVER_H

#include "HttpConn.h"
#include "HttpRouter.h"
#include "HttpLog.h"
#include "galay-http/utils/Http1_1ResponseBuilder.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/kernel/Runtime.h"
#include "galay-kernel/kernel/Coroutine.h"
#include <memory>
#include <atomic>
#include <functional>
#include <optional>

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
using HttpConnHandlerImpl = std::function<Coroutine(HttpConnImpl<SocketType>)>;

/**
 * @brief HTTP服务器配置
 */
struct HttpServerConfig
{
    std::string host = "0.0.0.0";
    uint16_t port = 8080;
    int backlog = 128;
    size_t io_scheduler_count = 0;
    size_t compute_scheduler_count = 0;
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
        : m_runtime(config.io_scheduler_count, config.compute_scheduler_count)
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

        m_handler = [this](HttpConnImpl<SocketType> conn) -> Coroutine {
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
                    if (error.code() == kConnectionClose) {
                        HTTP_LOG_WARN("[disconnect]");
                    } else if (error.code() == kRecvTimeOut || error.code() == kSendTimeOut || error.code() == kRequestTimeOut) {
                        HTTP_LOG_WARN("[timeout] [request] [{}]", error.message());
                    } else {
                        HTTP_LOG_ERROR("[recv] [fail] [{}]", error.message());
                    }
                    break;
                }

                HTTP_LOG_DEBUG("[req] [read-ok]");
                keep_alive = request.header().isKeepAlive() && !request.header().isConnectionClose();

                auto match = m_router->findHandler(request.header().method(), request.header().uri());

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
                    while (true) {
                        auto send_result = co_await writer.sendResponse(response);
                        if (!send_result || send_result.value()) break;
                    }

                    if (!keep_alive) {
                        break;
                    }
                    continue;
                }

                HTTP_LOG_DEBUG("[handler] [call]");
                if constexpr (std::is_same_v<SocketType, TcpSocket>) {
                    co_await (*match.handler)(conn, std::move(request)).wait();
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
                      m_config.io_scheduler_count == 0 ? "auto" : std::to_string(m_config.io_scheduler_count),
                      m_config.compute_scheduler_count == 0 ? "auto" : std::to_string(m_config.compute_scheduler_count));

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
                scheduler->spawn(serverLoop(scheduler));
            }
        }

        return true;
    }

    virtual Coroutine serverLoop(IOScheduler* scheduler) {
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
            scheduler->spawn(m_handler(std::move(conn)));
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

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/SslSocket.h"
#include "galay-ssl/SslContext.h"

/**
 * @brief HTTPS 服务器配置
 */
struct HttpsServerConfig
{
    std::string host = "0.0.0.0";
    uint16_t port = 443;
    int backlog = 128;
    size_t io_scheduler_count = 0;
    size_t compute_scheduler_count = 0;
    HttpReaderSetting reader_setting;
    HttpWriterSetting writer_setting;

    // SSL 配置
    std::string cert_path;          // 证书文件路径 (PEM 格式)
    std::string key_path;           // 私钥文件路径 (PEM 格式)
    std::string ca_path;            // CA 证书路径（可选）
    bool verify_peer = false;       // 是否验证客户端证书
    int verify_depth = 4;           // 证书链验证深度
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

    Coroutine serverLoop(IOScheduler* scheduler) override {
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
            scheduler->spawn(handleSslConnection(std::move(client_socket)));
        }

        co_return;
    }

private:
    Coroutine handleSslConnection(galay::ssl::SslSocket socket) {
        // 执行 SSL 握手
        while (!socket.isHandshakeCompleted()) {
            auto handshake_result = co_await socket.handshake();
            if (!handshake_result) {
                auto& err = handshake_result.error();
                // WantRead/WantWrite 表示需要继续握手
                if (err.code() == galay::ssl::SslErrorCode::kHandshakeWantRead ||
                    err.code() == galay::ssl::SslErrorCode::kHandshakeWantWrite) {
                    continue;
                }
                // 其他错误则退出
                HTTP_LOG_ERROR("[ssl] [handshake-fail] [{}]", err.message());
                co_await socket.close();
                co_return;
            }
            break;  // 握手成功
        }

        HTTP_LOG_DEBUG("[ssl] [handshake-ok]");

        // 创建连接并调用处理器
        HttpConnImpl<galay::ssl::SslSocket> conn(std::move(socket));
        co_await m_handler(std::move(conn)).wait();
    }

    static HttpServerConfig convertConfig(const HttpsServerConfig& config) {
        HttpServerConfig base_config;
        base_config.host = config.host;
        base_config.port = config.port;
        base_config.backlog = config.backlog;
        base_config.io_scheduler_count = config.io_scheduler_count;
        base_config.compute_scheduler_count = config.compute_scheduler_count;
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
#endif

} // namespace galay::http

#endif // GALAY_HTTP_SERVER_H
