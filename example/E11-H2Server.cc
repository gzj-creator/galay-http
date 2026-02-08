/**
 * @file E11-H2Server.cc
 * @brief h2 (HTTP/2 over TLS) 服务器示例
 * @details 演示如何使用 HTTP/2 over TLS (ALPN 协商)
 * 
 * 注意: 此示例需要 SSL 支持 (GALAY_HTTP_SSL_ENABLED)
 * 
 * 测试方法:
 *   # 使用 curl
 *   curl --http2 -k -v https://localhost:8443/
 *   
 *   # 使用 nghttp
 *   nghttp -v https://localhost:8443/
 */

#include <iostream>
#include <csignal>

// 检查 SSL 支持
#ifdef GALAY_HTTP_SSL_ENABLED

#include "galay-http/kernel/http2/Http2Conn.h"
#include "galay-http/kernel/http2/Http2Stream.h"
#include "galay-http/kernel/http/HttpLog.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/kernel/Runtime.h"
#include "galay-ssl/SslSocket.h"
#include "galay-ssl/SslContext.h"

using namespace galay::http2;
using namespace galay::kernel;
using namespace galay::async;
using namespace galay::ssl;

static std::atomic<bool> g_running{true};
static std::atomic<uint64_t> g_requests{0};

void signalHandler(int) {
    g_running = false;
}

/**
 * @brief h2 服务器配置
 */
struct H2ServerConfig
{
    std::string host = "0.0.0.0";
    uint16_t port = 8443;
    int backlog = 128;
    size_t io_scheduler_count = 4;
    
    // SSL 配置
    std::string cert_path;
    std::string key_path;
    
    // HTTP/2 设置
    uint32_t max_concurrent_streams = 100;
    uint32_t initial_window_size = 65535;
    bool enable_push = true;
};

/**
 * @brief 简化的 h2 服务器
 */
class H2Server
{
public:
    using RequestHandler = std::function<Coroutine(Http2ConnImpl<SslSocket>&, Http2Stream::ptr, Http2Request)>;
    
    explicit H2Server(const H2ServerConfig& config)
        : m_runtime(config.io_scheduler_count, 0)
        , m_config(config)
        , m_ssl_ctx(SslMethod::TLS_Server)
        , m_running(false)
    {
    }
    
    ~H2Server() { stop(); }
    
    void start(RequestHandler handler) {
        m_handler = std::move(handler);
        
        // 初始化 SSL
        if (!initSsl()) {
            throw std::runtime_error("Failed to initialize SSL");
        }
        
        m_runtime.start();
        
        m_listener = std::make_unique<TcpSocket>(IPType::IPV4);
        m_listener->option().handleReuseAddr();
        m_listener->option().handleNonBlock();
        
        Host bind_host(IPType::IPV4, m_config.host, m_config.port);
        m_listener->bind(bind_host);
        m_listener->listen(m_config.backlog);
        
        m_running = true;
        HTTP_LOG_INFO("[server] [listen] [h2] [https://{}:{}]", m_config.host, m_config.port);
        
        m_runtime.getNextIOScheduler()->spawn(serverLoop());
    }
    
    void stop() {
        if (!m_running) return;
        m_running = false;
        m_listener.reset();
        m_runtime.stop();
    }

private:
    bool initSsl() {
        if (!m_ssl_ctx.isValid()) return false;
        
        auto cert_result = m_ssl_ctx.loadCertificate(m_config.cert_path);
        if (!cert_result) {
            HTTP_LOG_ERROR("[ssl] [cert] [load-fail] [{}]", m_config.cert_path);
            return false;
        }
        
        auto key_result = m_ssl_ctx.loadPrivateKey(m_config.key_path);
        if (!key_result) {
            HTTP_LOG_ERROR("[ssl] [key] [load-fail] [{}]", m_config.key_path);
            return false;
        }
        
        // 设置 ALPN 回调以协商 h2
        m_ssl_ctx.setALPNProtocols({"h2", "http/1.1"});
        
        return true;
    }
    
    Coroutine serverLoop() {
        while (m_running) {
            Host client_host;
            auto accept_result = co_await m_listener->accept(&client_host);
            
            if (!accept_result) {
                if (m_running) {
                    HTTP_LOG_ERROR("[accept] [fail]");
                }
                continue;
            }
            
            HTTP_LOG_INFO("[connect] [h2] [{}:{}]", client_host.ip(), client_host.port());
            
            SslSocket ssl_socket(&m_ssl_ctx, accept_result.value());
            ssl_socket.option().handleNonBlock();
            
            m_runtime.getNextIOScheduler()->spawn(handleConnection(std::move(ssl_socket)));
        }
        co_return;
    }
    
    Coroutine handleConnection(SslSocket socket) {
        // SSL 握手
        while (!socket.isHandshakeCompleted()) {
            auto result = co_await socket.handshake();
            if (!result) {
                auto& err = result.error();
                if (err.code() == SslErrorCode::kHandshakeWantRead ||
                    err.code() == SslErrorCode::kHandshakeWantWrite) {
                    continue;
                }
                HTTP_LOG_ERROR("[ssl] [handshake-fail]");
                co_await socket.close();
                co_return;
            }
            break;
        }
        
        // 检查 ALPN 协商结果
        std::string alpn = socket.getALPNProtocol();
        if (alpn != "h2") {
            HTTP_LOG_WARN("[ssl] [alpn] [mismatch] [got={}] [expect=h2]", alpn);
            // 可以回退到 HTTP/1.1，这里简化处理
        }
        
        HTTP_LOG_DEBUG("[ssl] [handshake-ok] [alpn={}]", alpn);
        
        Http2ConnImpl<SslSocket> conn(std::move(socket));
        
        // 读取 Connection Preface
        char preface[24];
        size_t received = 0;
        while (received < 24) {
            auto result = co_await conn.socket().recv(preface + received, 24 - received);
            if (!result || result.value().size() == 0) {
                co_await conn.close();
                co_return;
            }
            received += result.value().size();
        }
        
        if (std::memcmp(preface, kHttp2ConnectionPreface.data(), 24) != 0) {
            HTTP_LOG_ERROR("[preface] [invalid]");
            co_await conn.close();
            co_return;
        }
        
        // 发送服务器 SETTINGS
        co_await conn.sendSettings();
        
        // 处理帧循环（简化版本）
        while (!conn.isGoawaySent() && !conn.isGoawayReceived()) {
            auto frame_result = co_await conn.readFrame();
            if (!frame_result) {
                break;
            }
            
            auto& frame = *frame_result;
            
            switch (frame->type()) {
                case Http2FrameType::Settings: {
                    auto* settings = static_cast<Http2SettingsFrame*>(frame.get());
                    if (!settings->isAck()) {
                        conn.peerSettings().applySettings(*settings);
                        co_await conn.sendSettingsAck();
                    }
                    break;
                }
                
                case Http2FrameType::Headers: {
                    auto* headers = static_cast<Http2HeadersFrame*>(frame.get());
                    uint32_t stream_id = frame->streamId();
                    
                    auto stream = conn.createStream(stream_id);
                    conn.setLastPeerStreamId(stream_id);
                    
                    auto decode_result = conn.decoder().decode(headers->headerBlock());
                    if (!decode_result) {
                        co_await conn.sendGoaway(Http2ErrorCode::CompressionError);
                        break;
                    }
                    
                    Http2Request request;
                    for (const auto& field : *decode_result) {
                        if (field.name == ":method") request.method = field.value;
                        else if (field.name == ":scheme") request.scheme = field.value;
                        else if (field.name == ":authority") request.authority = field.value;
                        else if (field.name == ":path") request.path = field.value;
                        else request.headers.push_back(field);
                    }
                    
                    stream->onHeadersReceived(headers->isEndStream());
                    
                    if (headers->isEndStream()) {
                        g_requests++;
                        m_runtime.getNextIOScheduler()->spawn(
                            m_handler(conn, stream, std::move(request)));
                    }
                    break;
                }
                
                case Http2FrameType::Data: {
                    auto* data = static_cast<Http2DataFrame*>(frame.get());
                    auto stream = conn.getStream(frame->streamId());
                    if (stream) {
                        stream->appendData(data->data());
                        stream->onDataReceived(data->isEndStream());
                        
                        if (data->isEndStream()) {
                            g_requests++;
                            Http2Request request = stream->request();
                            m_runtime.getNextIOScheduler()->spawn(
                                m_handler(conn, stream, std::move(request)));
                        }
                    }
                    break;
                }
                
                case Http2FrameType::Ping: {
                    auto* ping = static_cast<Http2PingFrame*>(frame.get());
                    if (!ping->isAck()) {
                        co_await conn.sendPing(ping->opaqueData(), true);
                    }
                    break;
                }
                
                case Http2FrameType::WindowUpdate: {
                    auto* wu = static_cast<Http2WindowUpdateFrame*>(frame.get());
                    if (frame->streamId() == 0) {
                        conn.adjustConnSendWindow(wu->windowSizeIncrement());
                    } else {
                        auto stream = conn.getStream(frame->streamId());
                        if (stream) {
                            stream->adjustSendWindow(wu->windowSizeIncrement());
                        }
                    }
                    break;
                }
                
                case Http2FrameType::GoAway:
                    conn.setGoawayReceived();
                    break;
                    
                default:
                    break;
            }
        }
        
        co_await conn.close();
        co_return;
    }

    Runtime m_runtime;
    H2ServerConfig m_config;
    RequestHandler m_handler;
    SslContext m_ssl_ctx;
    std::unique_ptr<TcpSocket> m_listener;
    std::atomic<bool> m_running;
};

// 请求处理器
Coroutine h2Handler(Http2ConnImpl<SslSocket>& conn, Http2Stream::ptr stream, Http2Request request) {
    HTTP_LOG_INFO("[h2] [req] [{}] [{}] [stream={}]", request.method, request.path, stream->streamId());
    
    Http2Response response;
    response.setStatus(200);
    response.setHeader("content-type", "text/html; charset=utf-8");
    response.setHeader("server", "Galay-H2/1.0");
    
    std::string html = R"(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>H2 Server (HTTPS)</title>
    <style>
        body { font-family: Arial, sans-serif; max-width: 800px; margin: 50px auto; padding: 20px; }
        h1 { color: #2e7d32; }
        .secure { color: #2e7d32; }
        pre { background: #f4f4f4; padding: 15px; border-radius: 5px; }
    </style>
</head>
<body>
    <h1>🔒 HTTP/2 over TLS (h2)</h1>
    <p class="secure">This connection is secured with TLS and using HTTP/2 protocol.</p>
    
    <h2>Request Info</h2>
    <pre>
Method: )" + request.method + R"(
Path: )" + request.path + R"(
Authority: )" + request.authority + R"(
    </pre>
    
    <h2>Test Commands</h2>
    <pre>
curl --http2 -k https://localhost:8443/
nghttp -v https://localhost:8443/
    </pre>
</body>
</html>)";
    
    response.setBody(html);

    // 构建响应头部
    Http2Headers headers;
    headers.status(response.status);
    for (const auto& h : response.headers) {
        headers.add(h.name, h.value);
    }

    bool has_body = !response.body.empty();

    // 发送 HEADERS（循环直到完成）
    while (true) {
        auto result = co_await conn.sendHeaders(stream->streamId(), headers, !has_body, true);
        if (!result) co_return;
        if (result.value()) break;
    }

    // 发送 DATA（如果有，循环直到完成）
    if (has_body) {
        while (true) {
            auto result = co_await conn.sendDataFrame(stream->streamId(), response.body, true);
            if (!result) co_return;
            if (result.value()) break;
        }
    }

    co_return;
}

int main(int argc, char* argv[]) {
    int port = 8443;
    std::string cert_path = "test.crt";
    std::string key_path = "test.key";
    
    if (argc > 1) port = std::atoi(argv[1]);
    if (argc > 2) cert_path = argv[2];
    if (argc > 3) key_path = argv[3];
    
    std::cout << "========================================\n";
    std::cout << "H2 (HTTP/2 over TLS) Server Example\n";
    std::cout << "========================================\n";
    
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    try {
        H2ServerConfig config;
        config.port = port;
        config.cert_path = cert_path;
        config.key_path = key_path;
        
        H2Server server(config);
        
        std::cout << "Server running on https://0.0.0.0:" << port << "\n";
        std::cout << "Test: curl --http2 -k https://localhost:" << port << "/\n";
        std::cout << "Press Ctrl+C to stop\n";
        std::cout << "========================================\n";
        
        server.start(h2Handler);
        
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        std::cout << "\nTotal requests: " << g_requests << "\n";
        server.stop();
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}

#else // !GALAY_HTTP_SSL_ENABLED

int main() {
    std::cout << "SSL support is not enabled.\n";
    std::cout << "Rebuild with -DGALAY_HTTP_ENABLE_SSL=ON\n";
    return 0;
}

#endif
