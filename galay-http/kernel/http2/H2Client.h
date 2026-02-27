#ifndef GALAY_H2_CLIENT_H
#define GALAY_H2_CLIENT_H

#include "Http2Conn.h"
#include "Http2Stream.h"
#include "galay-http/protoc/http2/Http2Base.h"
#include "galay-http/protoc/http2/Http2Frame.h"
#include "galay-http/kernel/http/HttpLog.h"
#include "galay-http/kernel/http/SslHandshakeAwaitable.h"
#include "galay-kernel/kernel/Coroutine.h"
#include "galay-kernel/kernel/Timeout.hpp"
#include "galay-ssl/async/SslSocket.h"
#include "galay-ssl/ssl/SslContext.h"
#include <memory>
#include <optional>

namespace galay::http2
{

using namespace galay::kernel;
using namespace galay::ssl;

/**
 * @brief H2 客户端配置
 */
struct H2ClientConfig
{
    uint32_t max_concurrent_streams = 100;
    uint32_t initial_window_size = 65535;
    uint32_t max_frame_size = 16384;
    uint32_t max_header_list_size = 8192;
    bool verify_peer = false;           // 是否验证服务器证书
    std::string ca_path;                // CA 证书路径（可选）
};

class H2ClientBuilder {
public:
    H2ClientBuilder& maxConcurrentStreams(uint32_t v)  { m_config.max_concurrent_streams = v; return *this; }
    H2ClientBuilder& initialWindowSize(uint32_t v)    { m_config.initial_window_size = v; return *this; }
    H2ClientBuilder& maxFrameSize(uint32_t v)         { m_config.max_frame_size = v; return *this; }
    H2ClientBuilder& maxHeaderListSize(uint32_t v)    { m_config.max_header_list_size = v; return *this; }
    H2ClientBuilder& verifyPeer(bool v)               { m_config.verify_peer = v; return *this; }
    H2ClientBuilder& caPath(std::string v)            { m_config.ca_path = std::move(v); return *this; }
    H2ClientConfig build() const                      { return m_config; }
private:
    H2ClientConfig m_config;
};

/**
 * @brief H2 客户端 (HTTP/2 over TLS)
 * @details 通过 TLS ALPN 协商使用 HTTP/2
 *
 * 使用方式:
 * @code
 * H2Client client;
 *
 * // 连接并完成 TLS 握手 + HTTP/2 协商
 * while (true) {
 *     auto result = co_await client.connect("example.com", 443);
 *     if (!result) { // error }
 *     if (result.value()) break;  // 连接成功
 * }
 *
 * // 发送请求
 * while (true) {
 *     auto result = co_await client.get("/api");
 *     if (!result) { // error }
 *     if (result.value()) {
 *         auto& response = *result.value();
 *         // 处理响应
 *         break;
 *     }
 * }
 *
 * co_await client.close();
 * @endcode
 */
class H2Client
{
public:
    /**
     * @brief H2 连接等待体
     */
    class ConnectAwaitable : public TimeoutSupport<ConnectAwaitable>
    {
    public:
        ConnectAwaitable(H2Client& client, const std::string& host, uint16_t port)
            : m_client(client)
            , m_host(host)
            , m_port(port)
        {
        }

        bool await_ready() const noexcept { return false; }

        template<typename Handle>
        bool await_suspend(Handle handle) {
            if (!m_started) {
                m_started = true;
                m_flow = runFlow(this);
            }
            return m_flow.wait().await_suspend(handle);
        }

        std::expected<std::optional<bool>, Http2ErrorCode> await_resume() {
            if (m_result.has_value() && !m_result->has_value()) {
                m_error = Http2ErrorCode::InternalError;
            }
            if (m_error.has_value()) {
                return std::unexpected(*m_error);
            }
            if (!m_done) {
                return std::nullopt;
            }
            return true;
        }

        bool isInvalid() const { return !m_started || m_done || m_error.has_value(); }

    private:
        static Coroutine runFlow(ConnectAwaitable* self) {
            auto& client = self->m_client;
            client.m_host = self->m_host;
            client.m_port = self->m_port;
            client.m_connected = false;
            client.m_conn.reset();

            client.m_socket = std::make_unique<SslSocket>(&client.m_ssl_ctx);
            client.m_socket->option().handleNonBlock();
            client.m_socket->setHostname(self->m_host);

            Host target(IPType::IPV4, self->m_host, self->m_port);
            auto connect_result = co_await client.m_socket->connect(target);
            if (!connect_result) {
                HTTP_LOG_ERROR("[h2] [connect-fail] [{}]", connect_result.error().message());
                self->m_error = Http2ErrorCode::ConnectError;
                self->m_done = true;
                co_return;
            }

            auto handshake_result = co_await galay::http::handshakeCompletely(*client.m_socket);
            if (!handshake_result) {
                HTTP_LOG_ERROR("[h2] [handshake-fail] [{}]", handshake_result.error().message());
                self->m_error = Http2ErrorCode::InternalError;
                self->m_done = true;
                co_return;
            }

            std::string alpn = client.m_socket->getALPNProtocol();
            client.m_alpn_protocol = alpn;
            if (alpn != "h2") {
                HTTP_LOG_ERROR("[h2] [alpn-fail] [got={}] [expect=h2]", alpn);
                self->m_error = Http2ErrorCode::ConnectError;
                self->m_done = true;
                co_return;
            }
            HTTP_LOG_DEBUG("[h2] [handshake-ok] [alpn={}]", alpn);

            client.m_conn = std::make_unique<Http2ConnImpl<SslSocket>>(
                std::move(*client.m_socket)
            );
            client.m_socket.reset();

            client.m_conn->localSettings().max_concurrent_streams = client.m_config.max_concurrent_streams;
            client.m_conn->localSettings().initial_window_size = client.m_config.initial_window_size;
            client.m_conn->localSettings().max_frame_size = client.m_config.max_frame_size;
            client.m_conn->localSettings().enable_push = 0;

            std::string preface(kHttp2ConnectionPreface.begin(), kHttp2ConnectionPreface.end());
            auto preface_result = co_await client.m_conn->writeRaw(std::move(preface));
            if (!preface_result) {
                self->m_error = Http2ErrorCode::InternalError;
                self->m_done = true;
                co_return;
            }

            auto settings_result = co_await client.m_conn->sendSettings();
            if (!settings_result) {
                self->m_error = Http2ErrorCode::InternalError;
                self->m_done = true;
                co_return;
            }

            while (true) {
                auto frame_result = co_await client.m_conn->readFrame();
                if (!frame_result) {
                    if (frame_result.error() == Http2ErrorCode::NoError) {
                        continue;
                    }
                    self->m_error = frame_result.error();
                    self->m_done = true;
                    co_return;
                }

                auto& frame = *frame_result;
                if (frame->type() != Http2FrameType::Settings) {
                    continue;
                }

                auto* settings = static_cast<Http2SettingsFrame*>(frame.get());
                if (settings->isAck()) {
                    continue;
                }

                auto err = client.m_conn->peerSettings().applySettings(*settings);
                if (err != Http2ErrorCode::NoError) {
                    self->m_error = err;
                    self->m_done = true;
                    co_return;
                }
                client.m_conn->encoder().setMaxTableSize(client.m_conn->peerSettings().header_table_size);
                break;
            }

            auto ack_result = co_await client.m_conn->sendSettingsAck();
            if (!ack_result) {
                self->m_error = Http2ErrorCode::InternalError;
                self->m_done = true;
                co_return;
            }

            client.m_connected = true;
            HTTP_LOG_INFO("[connect] [h2] [{}:{}]", client.m_host, client.m_port);
            self->m_done = true;
            co_return;
        }

        H2Client& m_client;
        std::string m_host;
        uint16_t m_port;
        bool m_started = false;
        bool m_done = false;
        std::optional<Http2ErrorCode> m_error;
        Coroutine m_flow;

    public:
        std::optional<std::expected<void, IOError>> m_result;
    };

    /**
     * @brief H2 请求等待体
     */
    class RequestAwaitable : public TimeoutSupport<RequestAwaitable>
    {
    public:
        RequestAwaitable(H2Client& client, Http2Request&& request)
            : m_client(client)
            , m_request(std::move(request))
        {
        }

        bool await_ready() const noexcept { return false; }

        template<typename Handle>
        bool await_suspend(Handle handle) {
            if (!m_started) {
                m_started = true;
                m_flow = runFlow(this);
            }
            return m_flow.wait().await_suspend(handle);
        }

        std::expected<std::optional<Http2Response>, Http2ErrorCode> await_resume() {
            if (m_result.has_value() && !m_result->has_value()) {
                return std::unexpected(Http2ErrorCode::InternalError);
            }
            if (m_error.has_value()) {
                return std::unexpected(*m_error);
            }
            if (!m_done) {
                return std::nullopt;
            }
            if (!m_response.has_value()) {
                return std::nullopt;
            }
            return std::move(*m_response);
        }

        bool isInvalid() const { return !m_started || m_done || m_error.has_value(); }

    private:
        static Coroutine runFlow(RequestAwaitable* self) {
            auto& client = self->m_client;
            if (!client.m_connected || !client.m_conn) {
                self->m_error = Http2ErrorCode::ConnectError;
                self->m_done = true;
                co_return;
            }

            self->m_stream_id = client.m_next_stream_id;
            client.m_next_stream_id += 2;
            client.m_conn->createStream(self->m_stream_id);

            std::vector<Http2HeaderField> headers;
            headers.push_back({":method", self->m_request.method});
            headers.push_back({":scheme", self->m_request.scheme});
            headers.push_back({":authority", self->m_request.authority});
            headers.push_back({":path", self->m_request.path.empty() ? "/" : self->m_request.path});
            for (const auto& h : self->m_request.headers) {
                headers.push_back(h);
            }

            bool end_stream = self->m_request.body.empty();
            auto send_headers_result = co_await client.m_conn->sendHeaders(self->m_stream_id, headers, end_stream, true);
            if (!send_headers_result) {
                self->m_error = Http2ErrorCode::InternalError;
                self->m_done = true;
                co_return;
            }

            if (!self->m_request.body.empty()) {
                auto send_data_result = co_await client.m_conn->sendDataFrame(self->m_stream_id, self->m_request.body, true);
                if (!send_data_result) {
                    self->m_error = Http2ErrorCode::InternalError;
                    self->m_done = true;
                    co_return;
                }
            }

            Http2Response response;
            while (true) {
                auto frame_result = co_await client.m_conn->readFrame();
                if (!frame_result) {
                    if (frame_result.error() == Http2ErrorCode::NoError) {
                        continue;
                    }
                    self->m_error = frame_result.error();
                    self->m_done = true;
                    co_return;
                }

                auto& frame = *frame_result;
                auto stream = client.m_conn->getStream(self->m_stream_id);

                switch (frame->type()) {
                    case Http2FrameType::Settings: {
                        auto* settings = static_cast<Http2SettingsFrame*>(frame.get());
                        if (!settings->isAck()) {
                            auto err = client.m_conn->peerSettings().applySettings(*settings);
                            if (err != Http2ErrorCode::NoError) {
                                co_await client.m_conn->sendGoaway(err);
                                co_return;
                            }
                        }
                        continue;
                    }
                    case Http2FrameType::Headers: {
                        auto* hdrs = static_cast<Http2HeadersFrame*>(frame.get());
                        if (frame->streamId() != self->m_stream_id) {
                            continue;
                        }

                        if (stream) {
                            stream->appendHeaderBlock(hdrs->headerBlock());
                            if (hdrs->isEndHeaders()) {
                                auto decode_result = client.m_conn->decoder().decode(stream->headerBlock());
                                if (!decode_result) {
                                    self->m_error = Http2ErrorCode::CompressionError;
                                    self->m_done = true;
                                    co_return;
                                }
                                stream->clearHeaderBlock();

                                for (const auto& field : *decode_result) {
                                    if (field.name == ":status") {
                                        try {
                                            response.status = std::stoi(field.value);
                                        } catch (...) {
                                            self->m_error = Http2ErrorCode::ProtocolError;
                                            self->m_done = true;
                                            co_return;
                                        }
                                    } else {
                                        response.headers.push_back(field);
                                    }
                                }

                                if (hdrs->isEndStream()) {
                                    self->m_response = std::move(response);
                                    self->m_done = true;
                                    co_return;
                                }
                            }
                        }
                        continue;
                    }
                    case Http2FrameType::Data: {
                        auto* data = static_cast<Http2DataFrame*>(frame.get());
                        if (frame->streamId() != self->m_stream_id) {
                            continue;
                        }

                        response.body.append(data->data());
                        if (data->isEndStream()) {
                            self->m_response = std::move(response);
                            self->m_done = true;
                            co_return;
                        }
                        continue;
                    }
                    case Http2FrameType::WindowUpdate:
                        continue;
                    case Http2FrameType::GoAway:
                        self->m_error = Http2ErrorCode::ProtocolError;
                        self->m_done = true;
                        co_return;
                    case Http2FrameType::RstStream:
                        if (frame->streamId() == self->m_stream_id) {
                            self->m_error = Http2ErrorCode::StreamClosed;
                            self->m_done = true;
                            co_return;
                        }
                        continue;
                    default:
                        continue;
                }
            }
        }

        H2Client& m_client;
        Http2Request m_request;
        std::optional<Http2Response> m_response;
        uint32_t m_stream_id = 0;
        bool m_started = false;
        bool m_done = false;
        std::optional<Http2ErrorCode> m_error;
        Coroutine m_flow;

    public:
        std::optional<std::expected<void, IOError>> m_result;
    };

public:
    H2Client(const H2ClientConfig& config = H2ClientConfig())
        : m_config(config)
        , m_connected(false)
        , m_next_stream_id(1)
        , m_ssl_ctx(SslMethod::TLS_Client)
    {
        // 设置 ALPN 协议列表，优先 h2
        m_ssl_ctx.setALPNProtocols({"h2"});

        // 配置证书验证
        if (config.verify_peer) {
            m_ssl_ctx.setVerifyMode(SslVerifyMode::Peer);
            if (!config.ca_path.empty()) {
                m_ssl_ctx.loadCACertificate(config.ca_path);
            } else {
                m_ssl_ctx.useDefaultCA();
            }
        } else {
            m_ssl_ctx.setVerifyMode(SslVerifyMode::None);
        }
    }

    ~H2Client() = default;

    H2Client(const H2Client&) = delete;
    H2Client& operator=(const H2Client&) = delete;

    /**
     * @brief 连接到服务器（包含 TLS 握手和 HTTP/2 协商）
     */
    ConnectAwaitable connect(const std::string& host, uint16_t port = 443) {
        return ConnectAwaitable(*this, host, port);
    }

    /**
     * @brief 发送 GET 请求
     */
    RequestAwaitable get(const std::string& path) {
        return createRequest("GET", path, "");
    }

    /**
     * @brief 发送 POST 请求
     */
    RequestAwaitable post(const std::string& path,
                            const std::string& body,
                            const std::string& content_type = "application/x-www-form-urlencoded") {
        return createRequest("POST", path, body, {{"content-type", content_type}});
    }

    /**
     * @brief 关闭连接
     */
    auto close() {
        m_connected = false;
        if (m_conn) {
            return m_conn->close();
        }
        if (m_socket) {
            return m_socket->close();
        }
        return m_dummy_socket.close();
    }

    bool isConnected() const { return m_connected; }

    /**
     * @brief 获取协商的 ALPN 协议
     */
    std::string getALPNProtocol() const {
        if (m_socket) {
            return m_socket->getALPNProtocol();
        }
        return m_alpn_protocol;
    }

private:
    RequestAwaitable createRequest(const std::string& method,
                                     const std::string& path,
                                     const std::string& body,
                                     const std::vector<Http2HeaderField>& headers = {}) {
        Http2Request req;
        req.method = method;
        req.path = path;
        req.body = body;
        req.scheme = "https";
        req.authority = m_host + ":" + std::to_string(m_port);
        req.headers = headers;
        return RequestAwaitable(*this, std::move(req));
    }

    H2ClientConfig m_config;
    std::string m_host;
    uint16_t m_port = 0;
    bool m_connected = false;
    uint32_t m_next_stream_id = 1;
    std::string m_alpn_protocol;

    SslContext m_ssl_ctx;
    SslSocket m_dummy_socket{nullptr};
    std::unique_ptr<SslSocket> m_socket;
    std::unique_ptr<Http2ConnImpl<SslSocket>> m_conn;
};

} // namespace galay::http2

#endif // GALAY_H2_CLIENT_H
