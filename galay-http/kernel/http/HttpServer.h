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
        return SocketType(fd);
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
#include "galay-socket/async/SslSocket.h"
#include "galay-http/ssl/SslConfig.h"

using HttpsConnHandler = HttpConnHandlerImpl<galay::async::SslSocket>;

/**
 * @brief HTTPS服务器类
 */
class HttpsServer : public HttpServerImpl<galay::async::SslSocket>
{
public:
    explicit HttpsServer(const galay::ssl::HttpsServerConfig& config)
        : HttpServerImpl<galay::async::SslSocket>(convertConfig(config))
        , m_ssl_config(config.ssl)
        , m_ssl_ctx(nullptr)
    {
    }

    ~HttpsServer() override {
        if (m_ssl_ctx) {
            SSL_CTX_free(static_cast<SSL_CTX*>(m_ssl_ctx));
            m_ssl_ctx = nullptr;
        }
    }

protected:
    bool startInternal() override {
        // 初始化 SSL 上下文
        if (!initSslContext()) {
            HTTP_LOG_ERROR("Failed to initialize SSL context");
            return false;
        }

        return HttpServerImpl<galay::async::SslSocket>::startInternal();
    }

    std::optional<galay::async::SslSocket> createClientSocket(int fd) override {
        if (!m_ssl_ctx) {
            HTTP_LOG_ERROR("SSL context not initialized");
            return std::nullopt;
        }

        // 创建 SSL 对象
        SSL* ssl = SSL_new(static_cast<SSL_CTX*>(m_ssl_ctx));
        if (!ssl) {
            HTTP_LOG_ERROR("Failed to create SSL object");
            return std::nullopt;
        }

        // 设置 fd
        SSL_set_fd(ssl, fd);

        // 执行 SSL 握手（服务器端）
        // 注意：实际的握手应该在异步环境中进行
        // 这里简化处理，实际实现需要使用 SslSocket 的异步握手

        return galay::async::SslSocket(fd, ssl);
    }

private:
    static HttpServerConfig convertConfig(const galay::ssl::HttpsServerConfig& config) {
        HttpServerConfig base_config;
        base_config.host = config.host;
        base_config.port = config.port;
        base_config.backlog = config.backlog;
        base_config.io_scheduler_count = config.io_scheduler_count;
        base_config.compute_scheduler_count = config.compute_scheduler_count;
        return base_config;
    }

    bool initSslContext() {
        // 创建 SSL 上下文
        const SSL_METHOD* method = TLS_server_method();
        m_ssl_ctx = SSL_CTX_new(method);
        if (!m_ssl_ctx) {
            HTTP_LOG_ERROR("Failed to create SSL_CTX");
            return false;
        }

        SSL_CTX* ctx = static_cast<SSL_CTX*>(m_ssl_ctx);

        // 设置选项
        SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);

        if (m_ssl_config.prefer_server_ciphers) {
            SSL_CTX_set_options(ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);
        }

        // 加载证书
        if (!m_ssl_config.cert_path.empty()) {
            if (SSL_CTX_use_certificate_file(ctx, m_ssl_config.cert_path.c_str(), SSL_FILETYPE_PEM) <= 0) {
                HTTP_LOG_ERROR("Failed to load certificate: {}", m_ssl_config.cert_path);
                return false;
            }
            HTTP_LOG_INFO("Loaded certificate: {}", m_ssl_config.cert_path);
        }

        // 加载私钥
        if (!m_ssl_config.key_path.empty()) {
            if (SSL_CTX_use_PrivateKey_file(ctx, m_ssl_config.key_path.c_str(), SSL_FILETYPE_PEM) <= 0) {
                HTTP_LOG_ERROR("Failed to load private key: {}", m_ssl_config.key_path);
                return false;
            }
            HTTP_LOG_INFO("Loaded private key: {}", m_ssl_config.key_path);

            // 验证私钥与证书匹配
            if (!SSL_CTX_check_private_key(ctx)) {
                HTTP_LOG_ERROR("Private key does not match certificate");
                return false;
            }
        }

        // 加载 CA 证书（用于验证客户端证书）
        if (!m_ssl_config.ca_path.empty() || !m_ssl_config.ca_dir.empty()) {
            const char* ca_file = m_ssl_config.ca_path.empty() ? nullptr : m_ssl_config.ca_path.c_str();
            const char* ca_dir = m_ssl_config.ca_dir.empty() ? nullptr : m_ssl_config.ca_dir.c_str();

            if (SSL_CTX_load_verify_locations(ctx, ca_file, ca_dir) <= 0) {
                HTTP_LOG_ERROR("Failed to load CA certificates");
                return false;
            }
            HTTP_LOG_INFO("Loaded CA certificates");
        }

        // 设置验证模式
        if (m_ssl_config.verify_peer) {
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
            SSL_CTX_set_verify_depth(ctx, m_ssl_config.verify_depth);
            HTTP_LOG_INFO("Client certificate verification enabled");
        } else {
            SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
        }

        // 设置加密套件
        if (!m_ssl_config.ciphers.empty()) {
            if (SSL_CTX_set_cipher_list(ctx, m_ssl_config.ciphers.c_str()) <= 0) {
                HTTP_LOG_ERROR("Failed to set cipher list: {}", m_ssl_config.ciphers);
                return false;
            }
        }

        HTTP_LOG_INFO("SSL context initialized successfully");
        return true;
    }

    galay::ssl::SslConfig m_ssl_config;
    void* m_ssl_ctx;  // SSL_CTX*
};
#endif

} // namespace galay::http

#endif // GALAY_HTTP_SERVER_H
