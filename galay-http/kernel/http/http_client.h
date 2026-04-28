#ifndef GALAY_HTTP_CLIENT_H
#define GALAY_HTTP_CLIENT_H

#include "http_session.h"
#include "http_log.h"
#include "galay-kernel/async/tcp_socket.h"
#include "galay-http/protoc/http/http_header.h"
#include <string>
#include <optional>
#include <regex>

namespace galay::websocket {
    template<typename SocketType>
    class WsConnImpl;
}

namespace galay::http
{

using namespace galay::async;
using namespace galay::kernel;

/**
 * @brief HTTP URL 解析结果
 */
struct HttpUrl {
    std::string scheme;
    std::string host;
    int port;
    std::string path;
    bool is_secure;

    static std::optional<HttpUrl> parse(const std::string& url) {
        std::regex url_regex(R"(^(http|https)://([^:/]+)(?::(\d+))?(/.*)?$)", std::regex::icase);
        std::smatch matches;

        if (!std::regex_match(url, matches, url_regex)) {
            HTTP_LOG_ERROR("[url] [invalid] [{}]", url);
            return std::nullopt;
        }

        HttpUrl result;
        result.scheme = matches[1].str();
        result.host = matches[2].str();
        result.is_secure = (result.scheme == "https" || result.scheme == "HTTPS");

        if (matches[3].matched) {
            try {
                result.port = std::stoi(matches[3].str());
            } catch (...) {
                HTTP_LOG_ERROR("[url] [port-invalid] [{}]", url);
                return std::nullopt;
            }
        } else {
            result.port = result.is_secure ? 443 : 80;
        }

        if (matches[4].matched) {
            result.path = matches[4].str();
        } else {
            result.path = "/";
        }

        return result;
    }
};

// 前向声明
template<typename SocketType>
class HttpClientImpl;

/**
 * @brief HTTP客户端配置
 * @details
 * - `header_mode` 控制 HeaderPair 的大小写/归一化策略。
 * - 配置会在 `build()` 时复制到客户端对象；后续修改 builder 不影响已构建实例。
 */
struct HttpClientConfig
{
    HeaderPair::Mode header_mode = HeaderPair::Mode::ClientSide;
};

/**
 * @brief HTTP 客户端 builder
 * @details builder 本身不持有 socket 或网络资源，只负责收集构造参数。
 */
class HttpClientBuilder {
public:
    HttpClientBuilder& headerMode(HeaderPair::Mode v) { m_config.header_mode = v; return *this; }
    HttpClientImpl<TcpSocket> build() const;
    HttpClientConfig buildConfig() const                       { return m_config; }
private:
    HttpClientConfig m_config;
};

/**
 * @brief HTTP客户端模板类
 * @details
 * 典型调用顺序：
 * 1. `connect(url)`
 * 2. `getSession()` 或直接访问 `socket()`
 * 3. 通过 `HttpSessionImpl` 发起请求/读取响应
 * 4. `close()`，或让底层 socket 在析构路径上释放
 *
 * 所有权说明：
 * - 客户端独占持有一个 `SocketType`
 * - `getSession()` 返回的 Session 只借用 socket，不转移所有权
 * - `releaseSocket()` 会把 socket 所有权转移给调用方，适合协议升级场景
 *
 * 失败语义：
 * - URL 非法、协议与客户端类型不匹配、或底层 socket 初始化失败时会抛出 `std::runtime_error`
 * - 网络连接结果通过返回的 awaitable / IO 结果对象反映
 */
template<typename SocketType>
class HttpClientImpl
{
public:
    HttpClientImpl(const HttpClientConfig& config = HttpClientConfig())
        : m_socket(nullptr)
        , m_config(config)
    {
    }

    HttpClientImpl(SocketType&& socket, const HttpClientConfig& config = HttpClientConfig())
        : m_socket(std::make_unique<SocketType>(std::move(socket)))
        , m_config(config)
    {
    }

    ~HttpClientImpl() = default;

    HttpClientImpl(const HttpClientImpl&) = delete;
    HttpClientImpl& operator=(const HttpClientImpl&) = delete;
    HttpClientImpl(HttpClientImpl&&) noexcept = default;
    HttpClientImpl& operator=(HttpClientImpl&&) noexcept = default;

    /**
     * @brief 解析 URL 并发起 TCP 连接
     * @param url 形如 `http://host[:port][/path]` 的目标地址
     * @return 底层 socket 的 connect awaitable；成功后可通过 `url()` 读取解析结果
     * @throws std::runtime_error URL 非法、协议与客户端类型不匹配、或 socket 初始化失败
     * @note 对明文 `HttpClient` 而言只接受 `http://`；若传入 `https://` 请改用 `HttpsClient`
     */
    auto connect(const std::string& url) {
        auto parsed_url = HttpUrl::parse(url);
        if (!parsed_url) {
            throw std::runtime_error("Invalid HTTP URL: " + url);
        }

        m_url = parsed_url.value();

        if constexpr (std::is_same_v<SocketType, TcpSocket>) {
            if (m_url.is_secure) {
                throw std::runtime_error("HTTPS requires HttpsClient");
            }
        }

        HTTP_LOG_INFO("[connect] [http] [{}:{}{}]", m_url.host, m_url.port, m_url.path);

        m_socket = std::make_unique<SocketType>(IPType::IPV4);

        auto nonblock_result = m_socket->option().handleNonBlock();
        if (!nonblock_result) {
            throw std::runtime_error("Failed to set non-blocking: " + nonblock_result.error().message());
        }

        Host server_host(IPType::IPV4, m_url.host, m_url.port);
        return m_socket->connect(server_host);
    }

    /**
     * @brief 创建一个借用当前 socket 的 HTTP session
     * @param ring_buffer_size Session 内部 RingBuffer 大小
     * @param reader_setting Reader 行为配置
     * @param writer_setting Writer 行为配置
     * @return 以值返回的 Session，对底层 socket 只有借用关系
     * @throws std::runtime_error 当前客户端尚未成功建立连接
     * @note 只要 Session 仍在使用，就必须保证客户端对象和内部 socket 继续存活
     */
    HttpSessionImpl<SocketType> getSession(size_t ring_buffer_size = 8192,
                                            const HttpReaderSetting& reader_setting = HttpReaderSetting(),
                                            const HttpWriterSetting& writer_setting = HttpWriterSetting()) {
        if (!m_socket) {
            throw std::runtime_error("Client not connected");
        }
        return HttpSessionImpl<SocketType>(*m_socket, ring_buffer_size, reader_setting, writer_setting);
    }

    /**
     * @brief 主动关闭底层 socket
     * @return 底层 socket 的 close awaitable
     * @note 如果已经通过 `releaseSocket()` 转移所有权，则不应再调用该函数
     */
    auto close() {
        return m_socket->close();
    }

    SocketType& socket() { return *m_socket; }
    const HttpUrl& url() const { return m_url; }

    /**
     * @brief 释放底层 socket 的所有权
     * @return 一个 `unique_ptr<SocketType>`；调用后客户端不再拥有 socket
     * @details 用于 HTTP -> WebSocket 等协议升级，调用方需负责后续关闭与生命周期管理
     */
    std::unique_ptr<SocketType> releaseSocket() { return std::move(m_socket); }

protected:
    std::unique_ptr<SocketType> m_socket;
    HttpClientConfig m_config;
    HttpUrl m_url;
};

// 类型别名 - HTTP (TcpSocket)
using HttpClient = HttpClientImpl<TcpSocket>;
inline HttpClient HttpClientBuilder::build() const { return HttpClient(m_config); }

} // namespace galay::http

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/async/ssl_socket.h"
#include "galay-ssl/ssl/ssl_context.h"

namespace galay::http {

/**
 * @brief HTTPS 客户端配置
 * @details
 * - `ca_path` 为空时不额外加载 CA 文件
 * - `verify_peer=false` 时不会校验证书链，适合本地自签名测试，不适合生产环境
 * - `header_mode` 与明文 `HttpClientConfig` 的语义保持一致
 */
struct HttpsClientConfig
{
    // SSL 配置
    std::string ca_path;            // CA 证书路径（可选，用于验证服务器）
    bool verify_peer = false;       // 是否验证服务器证书
    int verify_depth = 4;           // 证书链验证深度
    HeaderPair::Mode header_mode = HeaderPair::Mode::ClientSide;
};

class HttpsClient;

/**
 * @brief HTTPS 客户端 builder
 * @details builder 只负责收集 TLS 与 HTTP 头部策略配置，不直接建立网络连接。
 */
class HttpsClientBuilder {
public:
    HttpsClientBuilder& caPath(std::string v)              { m_config.ca_path = std::move(v); return *this; }
    HttpsClientBuilder& verifyPeer(bool v)                 { m_config.verify_peer = v; return *this; }
    HttpsClientBuilder& verifyDepth(int v)                 { m_config.verify_depth = v; return *this; }
    HttpsClientBuilder& headerMode(HeaderPair::Mode v) { m_config.header_mode = v; return *this; }
    HttpsClient build() const;
    HttpsClientConfig buildConfig() const                  { return m_config; }
private:
    HttpsClientConfig m_config;
};

/**
 * @brief HTTPS 客户端类
 * @details
 * 典型调用顺序：
 * 1. `connect(https_url)`
 * 2. `handshake()`
 * 3. `getSession()` 发起 HTTP 请求
 * 4. `close()` 或由上层协议关闭
 *
 * `connect()` 只负责 TCP 连接和 TLS socket 初始化，不会隐式完成 SSL 握手。
 * 若在握手前直接开始读写，会得到未定义的协议行为或底层错误。
 */
class HttpsClient : public HttpClientImpl<galay::ssl::SslSocket>
{
public:
    HttpsClient(const HttpsClientConfig& config = HttpsClientConfig())
        : HttpClientImpl<galay::ssl::SslSocket>(convertConfig(config))
        , m_https_config(config)
        , m_ssl_ctx(galay::ssl::SslMethod::TLS_Client)
    {
        initSslContext();
    }

    ~HttpsClient() = default;

    HttpsClient(const HttpsClient&) = delete;
    HttpsClient& operator=(const HttpsClient&) = delete;
    HttpsClient(HttpsClient&&) noexcept = default;
    HttpsClient& operator=(HttpsClient&&) noexcept = default;

    /**
     * @brief 解析 HTTPS URL、初始化 TLS socket 并发起 TCP 连接
     * @param url 形如 `https://host[:port][/path]` 的目标地址
     * @return 底层 TLS socket 的 connect awaitable
     * @throws std::runtime_error URL 非法或 socket 初始化失败
     * @note 该函数不会自动执行 TLS 握手；成功连接后仍需显式调用 `handshake()`
     */
    auto connect(const std::string& url) {
        auto parsed_url = HttpUrl::parse(url);
        if (!parsed_url) {
            throw std::runtime_error("Invalid HTTPS URL: " + url);
        }

        m_url = parsed_url.value();

        if (!m_url.is_secure) {
            HTTP_LOG_WARN("[https] [upgrade] [forced]");
        }

        HTTP_LOG_INFO("[connect] [https] [{}:{}{}]", m_url.host, m_url.port, m_url.path);

        // 正确的 SslSocket 构造方式
        m_socket = std::make_unique<galay::ssl::SslSocket>(&m_ssl_ctx);

        auto nonblock_result = m_socket->option().handleNonBlock();
        if (!nonblock_result) {
            throw std::runtime_error("Failed to set non-blocking: " + nonblock_result.error().message());
        }

        // 设置 SNI (Server Name Indication)
        auto sni_result = m_socket->setHostname(m_url.host);
        if (!sni_result) {
            HTTP_LOG_WARN("[sni] [fail] [{}]", sni_result.error().message());
        }

        Host server_host(IPType::IPV4, m_url.host, m_url.port);
        return m_socket->connect(server_host);
    }

    /**
     * @brief 执行 SSL 握手（协议完成后再唤醒）
     * @return 底层 TLS socket 的 handshake awaitable
     * @throws std::runtime_error 当前客户端尚未成功建立 TCP 连接
     */
    auto handshake() {
        if (!m_socket) {
            throw std::runtime_error("Client not connected");
        }
        return m_socket->handshake();
    }

    /**
     * @brief 检查握手是否完成
     * @return 已完成握手则返回 true；未连接或握手未完成则返回 false
     */
    bool isHandshakeCompleted() const {
        return m_socket && m_socket->isHandshakeCompleted();
    }

private:
    void initSslContext() {
        if (!m_ssl_ctx.isValid()) {
            throw std::runtime_error("Failed to create SSL context");
        }

        // 加载 CA 证书
        if (!m_https_config.ca_path.empty()) {
            auto result = m_ssl_ctx.loadCACertificate(m_https_config.ca_path);
            if (!result) {
                HTTP_LOG_WARN("[ssl] [ca] [load-fail] [{}]", m_https_config.ca_path);
            }
        }

        // 设置验证模式
        if (m_https_config.verify_peer) {
            m_ssl_ctx.setVerifyMode(galay::ssl::SslVerifyMode::Peer);
            m_ssl_ctx.setVerifyDepth(m_https_config.verify_depth);
        } else {
            m_ssl_ctx.setVerifyMode(galay::ssl::SslVerifyMode::None);
        }
    }

    static HttpClientConfig convertConfig(const HttpsClientConfig& config) {
        HttpClientConfig base_config;
        return base_config;
    }

    HttpsClientConfig m_https_config;
    galay::ssl::SslContext m_ssl_ctx;
};

inline HttpsClient HttpsClientBuilder::build() const { return HttpsClient(m_config); }

} // namespace galay::http
#endif

#endif // GALAY_HTTP_CLIENT_H
