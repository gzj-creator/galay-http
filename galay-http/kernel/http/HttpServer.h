#ifndef GALAY_HTTP_SERVER_H
#define GALAY_HTTP_SERVER_H

#include "HttpConn.h"
#include "HttpRouter.h"
#include "HttpLog.h"
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
    HttpReaderSetting reader_setting;
    HttpWriterSetting writer_setting;
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
        : m_runtime(LoadBalanceStrategy::ROUND_ROBIN, config.io_scheduler_count, config.compute_scheduler_count)
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
            bool keep_alive = true;

            while (keep_alive) {
                auto reader = conn.getReader();
                HttpRequest request;
                auto read_result = co_await reader.getRequest(request);

                if (!read_result) {
                    HTTP_LOG_ERROR("failed to read request: {}", read_result.error().message());
                    break;
                }

                keep_alive = request.header().isKeepAlive() && !request.header().isConnectionClose();

                auto match = m_router->findHandler(request.header().method(), request.header().uri());

                if (!match.handler) {
                    HTTP_LOG_WARN("no handler found for {} {}",
                                 static_cast<int>(request.header().method()),
                                 request.header().uri());

                    HttpResponse response;
                    response.header().version() = HttpVersion::HttpVersion_1_1;
                    response.header().code() = HttpStatusCode::OK_200;
                    response.header().headerPairs().addHeaderPair("Content-Type", "text/plain");
                    response.setBodyStr("404 Not Found");

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

                if constexpr (std::is_same_v<SocketType, TcpSocket>) {
                    co_await (*match.handler)(conn, std::move(request)).wait();
                } else {
                    HTTP_LOG_ERROR("Router mode not supported for HTTPS yet");
                    break;
                }

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
        HTTP_LOG_INFO("HTTP server stopping...");

        if (m_listener) {
            m_listener.reset();
        }

        m_runtime.stop();

        HTTP_LOG_INFO("HTTP server stopped");
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
            HTTP_LOG_WARN("server already running");
            return false;
        }

        if (!m_handler) {
            HTTP_LOG_ERROR("handler not set");
            return false;
        }

        HTTP_LOG_INFO("starting runtime with {} IO schedulers and {} compute schedulers",
                      m_config.io_scheduler_count == 0 ? "auto" : std::to_string(m_config.io_scheduler_count),
                      m_config.compute_scheduler_count == 0 ? "auto" : std::to_string(m_config.compute_scheduler_count));

        m_runtime.start();

        HTTP_LOG_INFO("runtime started with {} IO schedulers and {} compute schedulers",
                      m_runtime.getIOSchedulerCount(),
                      m_runtime.getComputeSchedulerCount());

        auto* scheduler = m_runtime.getNextIOScheduler();
        if (!scheduler) {
            HTTP_LOG_ERROR("no IO scheduler available");
            m_runtime.stop();
            return false;
        }

        m_listener = std::make_unique<TcpSocket>(IPType::IPV4);

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

        Host bind_host(IPType::IPV4, m_config.host, m_config.port);
        auto bind_result = m_listener->bind(bind_host);
        if (!bind_result) {
            HTTP_LOG_ERROR("failed to bind {}:{}: {}", m_config.host, m_config.port, bind_result.error().message());
            m_runtime.stop();
            return false;
        }

        auto listen_result = m_listener->listen(m_config.backlog);
        if (!listen_result) {
            HTTP_LOG_ERROR("failed to listen: {}", listen_result.error().message());
            m_runtime.stop();
            return false;
        }

        m_running.store(true);
        HTTP_LOG_INFO("HTTP server started on {}:{}", m_config.host, m_config.port);

        scheduler->spawn(serverLoop());

        return true;
    }

    virtual Coroutine serverLoop() {
        while (m_running.load()) {
            Host client_host;
            auto accept_result = co_await m_listener->accept(&client_host);

            if (!accept_result) {
                if (m_running.load()) {
                    HTTP_LOG_ERROR("accept failed: {}", accept_result.error().message());
                }
                continue;
            }

            HTTP_LOG_INFO("client connected from {}:{}", client_host.ip(), client_host.port());

            auto* scheduler = m_runtime.getNextIOScheduler();
            if (!scheduler) {
                HTTP_LOG_ERROR("no IO scheduler available");
                continue;
            }

            auto client_socket_opt = createClientSocket(accept_result.value());
            if (!client_socket_opt) {
                HTTP_LOG_ERROR("failed to create client socket");
                continue;
            }

            SocketType client_socket = std::move(*client_socket_opt);
            auto nonblock_result = client_socket.option().handleNonBlock();
            if (!nonblock_result) {
                HTTP_LOG_ERROR("failed to set client socket non-block: {}", nonblock_result.error().message());
                continue;
            }

            HttpConnImpl<SocketType> conn(std::move(client_socket), m_config.reader_setting, m_config.writer_setting);
            scheduler->spawn(m_handler(std::move(conn)));
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
            HTTP_LOG_ERROR("Failed to initialize SSL context");
            return false;
        }

        return HttpServerImpl<galay::ssl::SslSocket>::startInternal();
    }

    std::optional<galay::ssl::SslSocket> createClientSocket(GHandle fd) override {
        if (!m_ssl_ctx.isValid()) {
            HTTP_LOG_ERROR("SSL context not initialized");
            return std::nullopt;
        }

        return galay::ssl::SslSocket(&m_ssl_ctx, fd);
    }

    Coroutine serverLoop() override {
        while (m_running.load()) {
            Host client_host;
            auto accept_result = co_await m_listener->accept(&client_host);

            if (!accept_result) {
                if (m_running.load()) {
                    HTTP_LOG_ERROR("accept failed: {}", accept_result.error().message());
                }
                continue;
            }

            HTTP_LOG_INFO("HTTPS client connected from {}:{}", client_host.ip(), client_host.port());

            auto* scheduler = m_runtime.getNextIOScheduler();
            if (!scheduler) {
                HTTP_LOG_ERROR("no IO scheduler available");
                continue;
            }

            auto client_socket_opt = createClientSocket(accept_result.value());
            if (!client_socket_opt) {
                HTTP_LOG_ERROR("failed to create client SSL socket");
                continue;
            }

            galay::ssl::SslSocket client_socket = std::move(*client_socket_opt);
            auto nonblock_result = client_socket.option().handleNonBlock();
            if (!nonblock_result) {
                HTTP_LOG_ERROR("failed to set client socket non-block: {}", nonblock_result.error().message());
                continue;
            }

            // 在新协程中执行 SSL 握手和处理
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
                HTTP_LOG_ERROR("SSL handshake failed: {}", err.message());
                co_await socket.close();
                co_return;
            }
            break;  // 握手成功
        }

        HTTP_LOG_DEBUG("SSL handshake completed");

        // 创建连接并调用处理器
        HttpConnImpl<galay::ssl::SslSocket> conn(std::move(socket), m_config.reader_setting, m_config.writer_setting);
        co_await m_handler(std::move(conn)).wait();
    }

    static HttpServerConfig convertConfig(const HttpsServerConfig& config) {
        HttpServerConfig base_config;
        base_config.host = config.host;
        base_config.port = config.port;
        base_config.backlog = config.backlog;
        base_config.io_scheduler_count = config.io_scheduler_count;
        base_config.compute_scheduler_count = config.compute_scheduler_count;
        base_config.reader_setting = config.reader_setting;
        base_config.writer_setting = config.writer_setting;
        return base_config;
    }

    bool initSslContext() {
        if (!m_ssl_ctx.isValid()) {
            HTTP_LOG_ERROR("Failed to create SSL context");
            return false;
        }

        // 加载证书
        if (!m_https_config.cert_path.empty()) {
            auto result = m_ssl_ctx.loadCertificate(m_https_config.cert_path);
            if (!result) {
                HTTP_LOG_ERROR("Failed to load certificate: {} - {}",
                              m_https_config.cert_path, result.error().message());
                return false;
            }
            HTTP_LOG_INFO("Loaded certificate: {}", m_https_config.cert_path);
        }

        // 加载私钥
        if (!m_https_config.key_path.empty()) {
            auto result = m_ssl_ctx.loadPrivateKey(m_https_config.key_path);
            if (!result) {
                HTTP_LOG_ERROR("Failed to load private key: {} - {}",
                              m_https_config.key_path, result.error().message());
                return false;
            }
            HTTP_LOG_INFO("Loaded private key: {}", m_https_config.key_path);
        }

        // 加载 CA 证书
        if (!m_https_config.ca_path.empty()) {
            auto result = m_ssl_ctx.loadCACertificate(m_https_config.ca_path);
            if (!result) {
                HTTP_LOG_ERROR("Failed to load CA certificate: {}", m_https_config.ca_path);
                return false;
            }
            HTTP_LOG_INFO("Loaded CA certificate: {}", m_https_config.ca_path);
        }

        // 设置验证模式
        if (m_https_config.verify_peer) {
            m_ssl_ctx.setVerifyMode(galay::ssl::SslVerifyMode::Peer);
            m_ssl_ctx.setVerifyDepth(m_https_config.verify_depth);
            HTTP_LOG_INFO("Client certificate verification enabled");
        } else {
            m_ssl_ctx.setVerifyMode(galay::ssl::SslVerifyMode::None);
        }

        HTTP_LOG_INFO("SSL context initialized successfully");
        return true;
    }

    HttpsServerConfig m_https_config;
    galay::ssl::SslContext m_ssl_ctx;
};
#endif

} // namespace galay::http

#endif // GALAY_HTTP_SERVER_H
