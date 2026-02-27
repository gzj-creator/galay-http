#ifndef GALAY_HTTP_CLIENT_H
#define GALAY_HTTP_CLIENT_H

#include "HttpSession.h"
#include "HttpLog.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-http/protoc/http/HttpHeader.h"
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
 */
struct HttpClientConfig
{
    HeaderPair::NormalizeMode header_mode = HeaderPair::NormalizeMode::Canonical;
};

class HttpClientBuilder {
public:
    HttpClientBuilder& headerMode(HeaderPair::NormalizeMode v) { m_config.header_mode = v; return *this; }
    HttpClientImpl<TcpSocket> build() const;
    HttpClientConfig buildConfig() const                       { return m_config; }
private:
    HttpClientConfig m_config;
};

/**
 * @brief HTTP客户端模板类
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
    HttpClientImpl(HttpClientImpl&&) = delete;
    HttpClientImpl& operator=(HttpClientImpl&&) = delete;

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

    // 获取Session用于读写操作
    HttpSessionImpl<SocketType> getSession(size_t ring_buffer_size = 8192,
                                            const HttpReaderSetting& reader_setting = HttpReaderSetting(),
                                            const HttpWriterSetting& writer_setting = HttpWriterSetting()) {
        if (!m_socket) {
            throw std::runtime_error("Client not connected");
        }
        return HttpSessionImpl<SocketType>(*m_socket, ring_buffer_size, reader_setting, writer_setting);
    }

    auto close() {
        return m_socket->close();
    }

    SocketType& socket() { return *m_socket; }
    const HttpUrl& url() const { return m_url; }

    // 释放 socket 的所有权（用于协议升级）
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
#include "galay-ssl/async/SslSocket.h"
#include "galay-ssl/ssl/SslContext.h"
#include "SslHandshakeAwaitable.h"

namespace galay::http {

/**
 * @brief HTTPS 客户端配置
 */
struct HttpsClientConfig
{
    // SSL 配置
    std::string ca_path;            // CA 证书路径（可选，用于验证服务器）
    bool verify_peer = false;       // 是否验证服务器证书
    int verify_depth = 4;           // 证书链验证深度
    HeaderPair::NormalizeMode header_mode = HeaderPair::NormalizeMode::Canonical;
};

class HttpsClient;

class HttpsClientBuilder {
public:
    HttpsClientBuilder& caPath(std::string v)              { m_config.ca_path = std::move(v); return *this; }
    HttpsClientBuilder& verifyPeer(bool v)                 { m_config.verify_peer = v; return *this; }
    HttpsClientBuilder& verifyDepth(int v)                 { m_config.verify_depth = v; return *this; }
    HttpsClientBuilder& headerMode(HeaderPair::NormalizeMode v) { m_config.header_mode = v; return *this; }
    HttpsClient build() const;
    HttpsClientConfig buildConfig() const                  { return m_config; }
private:
    HttpsClientConfig m_config;
};

/**
 * @brief HTTPS 客户端类
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
    HttpsClient(HttpsClient&&) = delete;
    HttpsClient& operator=(HttpsClient&&) = delete;

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
     */
    auto handshake() {
        if (!m_socket) {
            throw std::runtime_error("Client not connected");
        }
        return handshakeCompletely(*m_socket);
    }

    /**
     * @brief 检查握手是否完成
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
