#ifndef GALAY_HTTP2_SERVER_H
#define GALAY_HTTP2_SERVER_H

#include "Http2Conn.h"
#include "Http2StreamManager.h"
#include "Http2Stream.h"
#include "galay-http/protoc/http2/Http2Base.h"
#include "galay-http/protoc/http2/Http2Frame.h"
#include "galay-http/protoc/http/HttpHeader.h"
#include "galay-http/kernel/http/HttpLog.h"
#include "galay-http/kernel/http/HttpConn.h"
#include "galay-http/kernel/http/SslHandshakeAwaitable.h"
#include "galay-http/utils/Http1_1ResponseBuilder.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/kernel/Runtime.h"
#include "galay-kernel/kernel/Coroutine.h"
#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/ssl/SslContext.h"
#include "galay-ssl/async/SslSocket.h"
#include <openssl/ssl.h>
#endif
#include <memory>
#include <atomic>
#include <functional>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <expected>
#include <string>
#include <array>

namespace galay::http2
{

using namespace galay::async;
using namespace galay::kernel;

/**
 * @brief h2c 服务器配置
 */
struct H2cServerConfig
{
    std::string host = "0.0.0.0";
    uint16_t port = 8080;
    int backlog = 128;
    size_t io_scheduler_count = 0;
    size_t compute_scheduler_count = 0;

    // HTTP/2 设置
    uint32_t max_concurrent_streams = 100;
    uint32_t initial_window_size = 65535;
    uint32_t max_frame_size = 16384;
    uint32_t max_header_list_size = 8192;
    bool enable_push = false;  // 默认禁用 Server Push（curl 不支持）
};

class H2cServerBuilder {
public:
    H2cServerBuilder& host(std::string v)              { m_config.host = std::move(v); return *this; }
    H2cServerBuilder& port(uint16_t v)                 { m_config.port = v; return *this; }
    H2cServerBuilder& backlog(int v)                   { m_config.backlog = v; return *this; }
    H2cServerBuilder& ioSchedulerCount(size_t v)       { m_config.io_scheduler_count = v; return *this; }
    H2cServerBuilder& computeSchedulerCount(size_t v)  { m_config.compute_scheduler_count = v; return *this; }
    H2cServerBuilder& maxConcurrentStreams(uint32_t v)  { m_config.max_concurrent_streams = v; return *this; }
    H2cServerBuilder& initialWindowSize(uint32_t v)    { m_config.initial_window_size = v; return *this; }
    H2cServerBuilder& maxFrameSize(uint32_t v)         { m_config.max_frame_size = v; return *this; }
    H2cServerBuilder& maxHeaderListSize(uint32_t v)    { m_config.max_header_list_size = v; return *this; }
    H2cServerBuilder& enablePush(bool v)               { m_config.enable_push = v; return *this; }
    H2cServerConfig build() const                      { return m_config; }
private:
    H2cServerConfig m_config;
};

/**
 * @brief 协议检测结果
 */
enum class DetectedProtocol {
    Unknown,            // 检测失败
    H2cPriorKnowledge,  // 直接 h2c
    H2cUpgrade,         // HTTP/1.1 Upgrade: h2c
    Http1,              // 普通 HTTP/1.1（降级）
};

/**
 * @brief 判断首字节是否像 HTTP method（大写 ASCII 字母）
 */
inline bool looksLikeHttpMethod(const char* buf) {
    return buf[0] >= 'A' && buf[0] <= 'Z';
}

/**
 * @brief 从 RingBuffer 的 iovec 拷贝 n 字节到 buf（不 consume）
 */
inline void peekRingBuffer(RingBuffer& rb, char* buf, size_t n) {
    auto iovecs = rb.getReadIovecs();
    size_t copied = 0;
    for (const auto& iov : iovecs) {
        size_t to_copy = std::min(iov.iov_len, n - copied);
        std::memcpy(buf + copied, iov.iov_base, to_copy);
        copied += to_copy;
        if (copied >= n) break;
    }
}

/**
 * @brief 把 RingBuffer 全部数据取出到 string 并 consume
 */
inline std::string drainRingBuffer(RingBuffer& rb) {
    std::string data;
    data.reserve(rb.readable());
    auto iovecs = rb.getReadIovecs();
    for (const auto& iov : iovecs) {
        data.append(static_cast<const char*>(iov.iov_base), iov.iov_len);
    }
    rb.consume(rb.readable());
    return data;
}

/**
 * @brief HTTP/2 流处理器类型（每个新流创建后 spawn handler(stream)）
 */
using Http2ConnectionHandler = Http2StreamHandler;

/**
 * @brief HTTP/1.1 降级处理器类型
 */
using Http1FallbackHandler = std::function<Coroutine(
    galay::http::HttpConnImpl<TcpSocket>, galay::http::HttpRequestHeader)>;

/**
 * @brief h2c 服务器 (HTTP/2 over cleartext)
 */
class H2cServer
{
public:
    explicit H2cServer(const H2cServerConfig& config = H2cServerConfig())
        : m_runtime(config.io_scheduler_count, config.compute_scheduler_count)
        , m_config(config)
        , m_handler(nullptr)
        , m_running(false)
    {
    }
    
    ~H2cServer() {
        stop();
    }
    
    H2cServer(const H2cServer&) = delete;
    H2cServer& operator=(const H2cServer&) = delete;
    
    void start(Http2ConnectionHandler handler) {
        m_handler = std::move(handler);
        startInternal();
    }

    void setHttp1Fallback(Http1FallbackHandler handler) {
        m_http1_fallback = std::move(handler);
    }
    
    void stop() {
        if (!m_running.load()) {
            return;
        }

        m_running.store(false);
        HTTP_LOG_INFO("[h2c] [server] [stopping]");

        m_runtime.stop();
        HTTP_LOG_INFO("[h2c] [server] [stopped]");
    }
    
    bool isRunning() const {
        return m_running.load();
    }
    
    Runtime& getRuntime() {
        return m_runtime;
    }

private:
    bool startInternal() {
        if (m_running.load()) {
            HTTP_LOG_WARN("[server] [already-running]");
            return false;
        }

        if (!m_handler) {
            HTTP_LOG_ERROR("[handler] [missing]");
            return false;
        }

        m_runtime.start();

        m_running.store(true);
        HTTP_LOG_INFO("[server] [listen] [h2c] [{}:{}]", m_config.host, m_config.port);

        // Spawn one serverLoop per IO scheduler with SO_REUSEPORT
        size_t io_scheduler_count = m_runtime.getIOSchedulerCount();
        for (size_t i = 0; i < io_scheduler_count; i++) {
            auto* scheduler = m_runtime.getIOScheduler(i);
            if (scheduler) {
                scheduler->spawn(serverLoop(scheduler));
            }
        }

        return true;
    }

    Coroutine serverLoop(IOScheduler* scheduler) {
        // Each serverLoop creates its own listener socket
        TcpSocket listener(IPType::IPV4);

        auto reuse_result = listener.option().handleReuseAddr();
        if (!reuse_result) {
            HTTP_LOG_ERROR("[socket] [reuseaddr-fail] [{}]", reuse_result.error().message());
            co_return;
        }

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

            HTTP_LOG_INFO("[connect] [h2c] [{}:{}]", client_host.ip(), client_host.port());

            TcpSocket client_socket(accept_result.value());
            auto nonblock_result = client_socket.option().handleNonBlock();
            if (!nonblock_result) {
                HTTP_LOG_ERROR("[socket] [nonblock-fail] [client] [{}]", nonblock_result.error().message());
                continue;
            }

            // Handle connection on the same scheduler
            scheduler->spawn(handleConnection(std::move(client_socket)));
        }

        co_return;
    }
    
    /**
     * @brief 处理新连接
     */
    Coroutine handleConnection(TcpSocket socket) {
        Http2ConnImpl<TcpSocket> conn(std::move(socket));

        // 配置本地设置
        conn.localSettings().from(m_config);

        DetectedProtocol protocol = DetectedProtocol::Unknown;
        galay::http::HttpRequestHeader upgrade_request;
        co_await detectProtocol(conn, protocol, upgrade_request).wait();

        switch (protocol) {
        case DetectedProtocol::H2cPriorKnowledge:
        case DetectedProtocol::H2cUpgrade: {
            // 初始化 StreamManager 并启动帧分发循环
            conn.initStreamManager();
            auto* mgr = conn.streamManager();
            HTTP_LOG_DEBUG("[h2] [stream-mgr] [starting]");
            co_await mgr->start(m_handler).wait();
            HTTP_LOG_DEBUG("[h2] [stream-mgr] [stopped]");
            co_await conn.close();
            break;
        }
        case DetectedProtocol::Http1:
            co_await handleHttp1Fallback(std::move(conn), std::move(upgrade_request)).wait();
            break;
        default:
            HTTP_LOG_ERROR("[protocol] [detect-fail]");
            co_await conn.close();
            break;
        }

        co_return;
    }

    Coroutine readAtLeast(Http2ConnImpl<TcpSocket>& conn, size_t n) {
        auto& rb = conn.ringBuffer();
        while (rb.readable() < n) {
            auto result = co_await conn.socket().readv(rb.getWriteIovecs());
            if (!result || result.value() == 0) {
                co_return;
            }
            rb.produce(result.value());
        }
        co_return;
    }

    /**
     * @brief 检测协议类型并完成初始握手
     * @param conn HTTP/2 连接
     * @param protocol 输出协议类型
     * @param upgrade_request 输出首个 HTTP/1.1 请求头（Upgrade/Http1 路径）
     */
    Coroutine detectProtocol(Http2ConnImpl<TcpSocket>& conn,
                            DetectedProtocol& protocol,
                            galay::http::HttpRequestHeader& upgrade_request) {
        protocol = DetectedProtocol::Unknown;
        auto& rb = conn.ringBuffer();

        co_await readAtLeast(conn, kHttp2ConnectionPrefaceLength).wait();
        if (rb.readable() < kHttp2ConnectionPrefaceLength) {
            co_return;
        }

        char peek_buf[kHttp2ConnectionPrefaceLength];
        peekRingBuffer(rb, peek_buf, kHttp2ConnectionPrefaceLength);

        // ===== Prior Knowledge =====
        if (std::memcmp(peek_buf, kHttp2ConnectionPreface.data(), kHttp2ConnectionPrefaceLength) == 0) {
            HTTP_LOG_DEBUG("[h2] [prior-knowledge]");
            rb.consume(kHttp2ConnectionPrefaceLength);

            auto settings_result = co_await conn.sendSettings();
            if (!settings_result) {
                co_return;
            }

            protocol = DetectedProtocol::H2cPriorKnowledge;
            co_return;
        }

        // ===== HTTP/1.1 (Upgrade or fallback) =====
        if (looksLikeHttpMethod(peek_buf)) {
            HTTP_LOG_DEBUG("[h1] [detect]");

            std::string header_data = drainRingBuffer(rb);
            while (header_data.find("\r\n\r\n") == std::string::npos && header_data.size() < 8192) {
                auto result = co_await conn.socket().readv(rb.getWriteIovecs());
                if (!result || result.value() == 0) {
                    co_return;
                }
                rb.produce(result.value());
                header_data.append(drainRingBuffer(rb));
            }

            size_t header_end = header_data.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                HTTP_LOG_ERROR("[header] [invalid] [too-large]");
                co_return;
            }

            auto parse_result = upgrade_request.fromString(
                std::string_view(header_data.data(), header_end + 4));
            if (parse_result.first != galay::http::kNoError || parse_result.second <= 0) {
                HTTP_LOG_ERROR("[header] [parse-fail]");
                co_return;
            }

            auto& headers = upgrade_request.headerPairs();
            std::string upgrade_value = headers.getValue("Upgrade");
            std::transform(upgrade_value.begin(), upgrade_value.end(), upgrade_value.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });

            bool has_upgrade = (upgrade_value == "h2c");
            bool has_http2_settings = headers.hasKey("HTTP2-Settings");

            if (has_upgrade && has_http2_settings) {
                HTTP_LOG_DEBUG("[h1] [upgrade] [h2c]");

                static constexpr char kUpgradeResp[] =
                    "HTTP/1.1 101 Switching Protocols\r\n"
                    "Connection: Upgrade\r\n"
                    "Upgrade: h2c\r\n"
                    "\r\n";
                static constexpr size_t kUpgradeRespLen = sizeof(kUpgradeResp) - 1;

                size_t sent = 0;
                while (sent < kUpgradeRespLen) {
                    auto send_result = co_await conn.socket().send(
                        kUpgradeResp + sent, kUpgradeRespLen - sent);
                    if (!send_result) {
                        HTTP_LOG_ERROR("[upgrade] [send-fail]");
                        co_return;
                    }
                    sent += send_result.value();
                }
                HTTP_LOG_DEBUG("[upgrade] [101-sent]");

                // HTTP 头后面可能已带部分 Connection Preface，写入 RingBuffer
                if (header_data.size() > header_end + 4) {
                    rb.write(header_data.data() + header_end + 4,
                             header_data.size() - header_end - 4);
                }

                co_await readAtLeast(conn, kHttp2ConnectionPrefaceLength).wait();
                if (rb.readable() < kHttp2ConnectionPrefaceLength) {
                    HTTP_LOG_ERROR("[preface] [recv-fail]");
                    co_return;
                }

                peekRingBuffer(rb, peek_buf, kHttp2ConnectionPrefaceLength);
                if (std::memcmp(peek_buf, kHttp2ConnectionPreface.data(), kHttp2ConnectionPrefaceLength) != 0) {
                    HTTP_LOG_ERROR("[preface] [invalid] [after-upgrade]");
                    co_return;
                }
                HTTP_LOG_DEBUG("[preface] [ok]");

                rb.consume(kHttp2ConnectionPrefaceLength);

                auto settings_result = co_await conn.sendSettings();
                if (!settings_result) {
                    co_return;
                }

                protocol = DetectedProtocol::H2cUpgrade;
                co_return;
            }

            protocol = DetectedProtocol::Http1;
            co_return;
        }

        HTTP_LOG_WARN("[protocol] [unknown] [h2c]");
        co_return;
    }

    Coroutine handleHttp1Fallback(Http2ConnImpl<TcpSocket>&& h2_conn,
                                  galay::http::HttpRequestHeader first_request_header) {
        galay::http::HttpConnImpl<TcpSocket> conn(
            std::move(h2_conn.socket()), std::move(h2_conn.ringBuffer()));

        if (m_http1_fallback) {
            co_await m_http1_fallback(std::move(conn), std::move(first_request_header)).wait();
            co_return;
        }

        auto writer = conn.getWriter();
        auto response = galay::http::Http1_1ResponseBuilder()
            .status(galay::http::HttpStatusCode::HttpVersionNotSupported_505)
            .body("505 HTTP Version Not Supported - Use h2c")
            .buildMove();
        auto result = co_await writer.sendResponse(response);
        if (!result) {
            HTTP_LOG_ERROR("[h1-fallback] [send-fail] [{}]", result.error().message());
        }
        co_await conn.close();
        co_return;
    }


private:
    Runtime m_runtime;
    H2cServerConfig m_config;
    Http2ConnectionHandler m_handler;
    Http1FallbackHandler m_http1_fallback;
    std::atomic<bool> m_running;
};

#ifdef GALAY_HTTP_SSL_ENABLED
/**
 * @brief h2 服务器配置（HTTP/2 over TLS）
 */
struct H2ServerConfig
{
    std::string host = "0.0.0.0";
    uint16_t port = 9443;
    int backlog = 128;
    size_t io_scheduler_count = 0;
    size_t compute_scheduler_count = 0;

    // SSL 配置
    std::string cert_path;
    std::string key_path;
    std::string ca_path;
    bool verify_peer = false;
    int verify_depth = 4;

    // HTTP/2 设置
    uint32_t max_concurrent_streams = 100;
    uint32_t initial_window_size = 65535;
    uint32_t max_frame_size = 16384;
    uint32_t max_header_list_size = 8192;
    bool enable_push = false;
};

class H2ServerBuilder {
public:
    H2ServerBuilder& host(std::string v)              { m_config.host = std::move(v); return *this; }
    H2ServerBuilder& port(uint16_t v)                 { m_config.port = v; return *this; }
    H2ServerBuilder& backlog(int v)                   { m_config.backlog = v; return *this; }
    H2ServerBuilder& ioSchedulerCount(size_t v)       { m_config.io_scheduler_count = v; return *this; }
    H2ServerBuilder& computeSchedulerCount(size_t v)  { m_config.compute_scheduler_count = v; return *this; }
    H2ServerBuilder& certPath(std::string v)          { m_config.cert_path = std::move(v); return *this; }
    H2ServerBuilder& keyPath(std::string v)           { m_config.key_path = std::move(v); return *this; }
    H2ServerBuilder& caPath(std::string v)            { m_config.ca_path = std::move(v); return *this; }
    H2ServerBuilder& verifyPeer(bool v)               { m_config.verify_peer = v; return *this; }
    H2ServerBuilder& verifyDepth(int v)               { m_config.verify_depth = v; return *this; }
    H2ServerBuilder& maxConcurrentStreams(uint32_t v) { m_config.max_concurrent_streams = v; return *this; }
    H2ServerBuilder& initialWindowSize(uint32_t v)    { m_config.initial_window_size = v; return *this; }
    H2ServerBuilder& maxFrameSize(uint32_t v)         { m_config.max_frame_size = v; return *this; }
    H2ServerBuilder& maxHeaderListSize(uint32_t v)    { m_config.max_header_list_size = v; return *this; }
    H2ServerBuilder& enablePush(bool v)               { m_config.enable_push = v; return *this; }
    H2ServerConfig build() const                      { return m_config; }
private:
    H2ServerConfig m_config;
};

/**
 * @brief h2 服务器 (HTTP/2 over TLS)
 */
class H2Server
{
public:
    explicit H2Server(const H2ServerConfig& config = H2ServerConfig())
        : m_runtime(config.io_scheduler_count, config.compute_scheduler_count)
        , m_config(config)
        , m_handler(nullptr)
        , m_running(false)
        , m_ssl_ctx(galay::ssl::SslMethod::TLS_Server)
    {
    }

    ~H2Server() {
        stop();
    }

    H2Server(const H2Server&) = delete;
    H2Server& operator=(const H2Server&) = delete;

    void start(Http2ConnectionHandler handler) {
        m_handler = std::move(handler);
        startInternal();
    }

    void stop() {
        if (!m_running.load()) {
            return;
        }

        m_running.store(false);
        HTTP_LOG_INFO("[h2] [server] [stopping]");
        m_runtime.stop();
        HTTP_LOG_INFO("[h2] [server] [stopped]");
    }

    bool isRunning() const {
        return m_running.load();
    }

    Runtime& getRuntime() {
        return m_runtime;
    }

private:
    static int selectH2AlpnCallback(SSL*,
                                    const unsigned char** out,
                                    unsigned char* outlen,
                                    const unsigned char* in,
                                    unsigned int inlen,
                                    void*) {
        // `in` is a vector of 8-bit length-prefixed protocols.
        unsigned int i = 0;
        while (i < inlen) {
            unsigned int len = in[i++];
            if (i + len > inlen) {
                return SSL_TLSEXT_ERR_NOACK;
            }
            if (len == 2 && in[i] == 'h' && in[i + 1] == '2') {
                *out = in + i;
                *outlen = 2;
                return SSL_TLSEXT_ERR_OK;
            }
            i += len;
        }
        return SSL_TLSEXT_ERR_NOACK;
    }

    bool startInternal() {
        if (m_running.load()) {
            HTTP_LOG_WARN("[h2] [server] [already-running]");
            return false;
        }
        if (!m_handler) {
            HTTP_LOG_ERROR("[h2] [handler] [missing]");
            return false;
        }
        if (!initSslContext()) {
            HTTP_LOG_ERROR("[h2] [ssl] [context-init-fail]");
            return false;
        }

        m_runtime.start();
        m_running.store(true);
        HTTP_LOG_INFO("[server] [listen] [h2] [{}:{}]", m_config.host, m_config.port);

        size_t io_scheduler_count = m_runtime.getIOSchedulerCount();
        for (size_t i = 0; i < io_scheduler_count; i++) {
            auto* scheduler = m_runtime.getIOScheduler(i);
            if (scheduler) {
                scheduler->spawn(serverLoop(scheduler));
            }
        }
        return true;
    }

    bool initSslContext() {
        if (!m_ssl_ctx.isValid()) {
            HTTP_LOG_ERROR("[h2] [ssl] [context-invalid]");
            return false;
        }

        if (m_config.cert_path.empty() || m_config.key_path.empty()) {
            HTTP_LOG_ERROR("[h2] [ssl] [missing-cert-or-key]");
            return false;
        }

        auto cert_result = m_ssl_ctx.loadCertificate(m_config.cert_path);
        if (!cert_result) {
            HTTP_LOG_ERROR("[h2] [ssl] [cert-load-fail] [{}] [{}]",
                           m_config.cert_path, cert_result.error().message());
            return false;
        }

        auto key_result = m_ssl_ctx.loadPrivateKey(m_config.key_path);
        if (!key_result) {
            HTTP_LOG_ERROR("[h2] [ssl] [key-load-fail] [{}] [{}]",
                           m_config.key_path, key_result.error().message());
            return false;
        }

        if (!m_config.ca_path.empty()) {
            auto ca_result = m_ssl_ctx.loadCACertificate(m_config.ca_path);
            if (!ca_result) {
                HTTP_LOG_ERROR("[h2] [ssl] [ca-load-fail] [{}] [{}]",
                               m_config.ca_path, ca_result.error().message());
                return false;
            }
        }

        if (m_config.verify_peer) {
            m_ssl_ctx.setVerifyMode(galay::ssl::SslVerifyMode::Peer);
            m_ssl_ctx.setVerifyDepth(m_config.verify_depth);
        } else {
            m_ssl_ctx.setVerifyMode(galay::ssl::SslVerifyMode::None);
        }

        auto alpn_result = m_ssl_ctx.setALPNProtocols({"h2"});
        if (!alpn_result) {
            HTTP_LOG_ERROR("[h2] [ssl] [alpn-set-fail] [{}]", alpn_result.error().message());
            return false;
        }
        SSL_CTX_set_alpn_select_cb(m_ssl_ctx.native(), &H2Server::selectH2AlpnCallback, nullptr);

        HTTP_LOG_INFO("[h2] [ssl] [context-ready]");
        return true;
    }

    Coroutine serverLoop(IOScheduler* scheduler) {
        TcpSocket listener(IPType::IPV4);

        auto reuse_result = listener.option().handleReuseAddr();
        if (!reuse_result) {
            HTTP_LOG_ERROR("[socket] [reuseaddr-fail] [{}]", reuse_result.error().message());
            co_return;
        }

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

            galay::ssl::SslSocket client_socket(&m_ssl_ctx, accept_result.value());
            auto nonblock_result = client_socket.option().handleNonBlock();
            if (!nonblock_result) {
                HTTP_LOG_ERROR("[socket] [nonblock-fail] [h2] [{}]", nonblock_result.error().message());
                continue;
            }

            scheduler->spawn(handleConnection(std::move(client_socket)));
        }

        co_return;
    }

    Coroutine readConnectionPreface(galay::ssl::SslSocket& socket,
                                    std::array<char, kHttp2ConnectionPrefaceLength>& preface,
                                    bool& ok) {
        ok = false;
        size_t received = 0;
        while (received < preface.size()) {
            auto recv_result = co_await socket.recv(preface.data() + received, preface.size() - received);
            if (!recv_result || recv_result.value().size() == 0) {
                co_return;
            }
            received += recv_result.value().size();
        }
        ok = true;
        co_return;
    }

    Coroutine handleConnection(galay::ssl::SslSocket socket) {
        auto handshake_result = co_await galay::http::handshakeCompletely(socket);
        if (!handshake_result) {
            HTTP_LOG_ERROR("[h2] [handshake-fail] [{}]", handshake_result.error().message());
            co_await socket.close();
            co_return;
        }

        std::string alpn = socket.getALPNProtocol();
        if (alpn != "h2") {
            HTTP_LOG_ERROR("[h2] [alpn-mismatch] [got={}]", alpn);
            co_await socket.close();
            co_return;
        }
        HTTP_LOG_DEBUG("[h2] [alpn-ok]");

        std::array<char, kHttp2ConnectionPrefaceLength> preface{};
        bool preface_ok = false;
        co_await readConnectionPreface(socket, preface, preface_ok).wait();
        if (!preface_ok ||
            std::memcmp(preface.data(), kHttp2ConnectionPreface.data(), kHttp2ConnectionPrefaceLength) != 0) {
            HTTP_LOG_ERROR("[h2] [preface-invalid]");
            co_await socket.close();
            co_return;
        }

        Http2ConnImpl<galay::ssl::SslSocket> conn(std::move(socket));
        conn.localSettings().from(m_config);

        auto settings_result = co_await conn.sendSettings();
        if (!settings_result) {
            HTTP_LOG_ERROR("[h2] [settings-send-fail] [code={}]", static_cast<int>(settings_result.error()));
            co_await conn.close();
            co_return;
        }

        conn.initStreamManager();
        auto* mgr = conn.streamManager();
        HTTP_LOG_DEBUG("[h2] [stream-mgr] [starting]");
        co_await mgr->start(m_handler).wait();
        HTTP_LOG_DEBUG("[h2] [stream-mgr] [stopped]");
        co_await conn.close();
        co_return;
    }

private:
    Runtime m_runtime;
    H2ServerConfig m_config;
    Http2ConnectionHandler m_handler;
    std::atomic<bool> m_running;
    galay::ssl::SslContext m_ssl_ctx;
};
#endif

} // namespace galay::http2

#endif // GALAY_HTTP2_SERVER_H
