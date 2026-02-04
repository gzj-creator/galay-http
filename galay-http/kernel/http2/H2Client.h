#ifndef GALAY_H2_CLIENT_H
#define GALAY_H2_CLIENT_H

#include "Http2Conn.h"
#include "Http2Stream.h"
#include "galay-http/protoc/http2/Http2Base.h"
#include "galay-http/protoc/http2/Http2Frame.h"
#include "galay-http/kernel/http/HttpLog.h"
#include "galay-kernel/kernel/Timeout.hpp"
#include "galay-ssl/async/SslSocket.h"
#include "galay-ssl/SslContext.h"
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
            , m_state(State::Invalid)
        {
        }

        bool await_ready() const noexcept { return false; }

        template<typename Handle>
        auto await_suspend(Handle handle) {
            switch (m_state) {
                case State::Invalid: {
                    m_state = State::Connecting;
                    m_client.m_host = m_host;
                    m_client.m_port = m_port;

                    // 创建 SSL Socket
                    m_client.m_socket = std::make_unique<SslSocket>(&m_client.m_ssl_ctx);
                    m_client.m_socket->option().handleNonBlock();

                    // 设置 SNI
                    m_client.m_socket->setHostname(m_host);

                    Host target(IPType::IPV4, m_host, m_port);
                    m_connect_awaitable.emplace(m_client.m_socket->connect(target));
                    return m_connect_awaitable->await_suspend(handle);
                }
                case State::Connecting:
                    return m_connect_awaitable->await_suspend(handle);
                case State::Handshaking:
                    if (!m_handshake_awaitable.has_value()) {
                        m_handshake_awaitable.emplace(m_client.m_socket->handshake());
                    }
                    return m_handshake_awaitable->await_suspend(handle);
                case State::SendingPreface: {
                    std::string preface(kHttp2ConnectionPreface.begin(), kHttp2ConnectionPreface.end());
                    m_write_awaitable.emplace(m_client.m_conn->writeRaw(std::move(preface)));
                    return m_write_awaitable->await_suspend(handle);
                }
                case State::SendingSettings:
                    m_write_awaitable.emplace(m_client.m_conn->sendSettings());
                    return m_write_awaitable->await_suspend(handle);
                case State::ReceivingSettings:
                    m_read_awaitable.emplace(m_client.m_conn->readFrame());
                    return m_read_awaitable->await_suspend(handle);
                case State::SendingSettingsAck:
                    m_write_awaitable.emplace(m_client.m_conn->sendSettingsAck());
                    return m_write_awaitable->await_suspend(handle);
                default:
                    return false;
            }
        }

        std::expected<std::optional<bool>, Http2ErrorCode> await_resume() {
            if (m_result.has_value() && !m_result->has_value()) {
                m_state = State::Invalid;
                return std::unexpected(Http2ErrorCode::InternalError);
            }

            switch (m_state) {
                case State::Connecting: {
                    auto result = m_connect_awaitable->await_resume();
                    if (!result) {
                        m_connect_awaitable.reset();
                        m_state = State::Invalid;
                        HTTP_LOG_ERROR("[h2] [connect-fail] [{}]", result.error().message());
                        return std::unexpected(Http2ErrorCode::ConnectError);
                    }
                    m_connect_awaitable.reset();
                    m_state = State::Handshaking;
                    return std::nullopt;
                }
                case State::Handshaking: {
                    auto result = m_handshake_awaitable->await_resume();
                    if (!result) {
                        auto& err = result.error();
                        // WantRead/WantWrite 表示需要继续握手
                        if (err.code() == SslErrorCode::kHandshakeWantRead ||
                            err.code() == SslErrorCode::kHandshakeWantWrite) {
                            m_handshake_awaitable.reset();
                            return std::nullopt;
                        }
                        m_handshake_awaitable.reset();
                        m_state = State::Invalid;
                        HTTP_LOG_ERROR("[h2] [handshake-fail] [{}]", err.message());
                        return std::unexpected(Http2ErrorCode::InternalError);
                    }
                    m_handshake_awaitable.reset();

                    // 检查 ALPN 协商结果
                    std::string alpn = m_client.m_socket->getALPNProtocol();
                    m_client.m_alpn_protocol = alpn;
                    if (alpn != "h2") {
                        HTTP_LOG_WARN("[h2] [alpn-fail] [got={}] [expect=h2]", alpn);
                    }

                    HTTP_LOG_DEBUG("[h2] [handshake-ok] [alpn={}]", alpn);

                    // 创建 Http2Conn
                    m_client.m_conn = std::make_unique<Http2ConnImpl<SslSocket>>(
                        std::move(*m_client.m_socket)
                    );
                    m_client.m_socket.reset();

                    // 配置设置
                    m_client.m_conn->localSettings().max_concurrent_streams = m_client.m_config.max_concurrent_streams;
                    m_client.m_conn->localSettings().initial_window_size = m_client.m_config.initial_window_size;
                    m_client.m_conn->localSettings().max_frame_size = m_client.m_config.max_frame_size;
                    m_client.m_conn->localSettings().enable_push = 0;

                    m_state = State::SendingPreface;
                    return std::nullopt;
                }
                case State::SendingPreface: {
                    auto result = m_write_awaitable->await_resume();
                    m_write_awaitable.reset();
                    if (!result) {
                        m_state = State::Invalid;
                        return std::unexpected(Http2ErrorCode::InternalError);
                    }
                    if (!result.value()) return std::nullopt;
                    m_state = State::SendingSettings;
                    return std::nullopt;
                }
                case State::SendingSettings: {
                    auto result = m_write_awaitable->await_resume();
                    m_write_awaitable.reset();
                    if (!result) {
                        m_state = State::Invalid;
                        return std::unexpected(Http2ErrorCode::InternalError);
                    }
                    if (!result.value()) return std::nullopt;
                    m_state = State::ReceivingSettings;
                    return std::nullopt;
                }
                case State::ReceivingSettings: {
                    auto result = m_read_awaitable->await_resume();
                    m_read_awaitable.reset();
                    if (!result) {
                        if (result.error() == Http2ErrorCode::NoError) return std::nullopt;
                        m_state = State::Invalid;
                        return std::unexpected(result.error());
                    }
                    auto& frame = *result;
                    if (frame->type() == Http2FrameType::Settings) {
                        auto* settings = static_cast<Http2SettingsFrame*>(frame.get());
                        if (!settings->isAck()) {
                            m_client.m_conn->peerSettings().applySettings(*settings);
                            m_client.m_conn->encoder().setMaxTableSize(m_client.m_conn->peerSettings().header_table_size);
                            m_state = State::SendingSettingsAck;
                            return std::nullopt;
                        }
                    }
                    return std::nullopt;
                }
                case State::SendingSettingsAck: {
                    auto result = m_write_awaitable->await_resume();
                    m_write_awaitable.reset();
                    if (!result) {
                        m_state = State::Invalid;
                        return std::unexpected(Http2ErrorCode::InternalError);
                    }
                    if (!result.value()) return std::nullopt;
                    m_client.m_connected = true;
                    m_state = State::Invalid;
                    HTTP_LOG_INFO("[connect] [h2] [{}:{}]", m_client.m_host, m_client.m_port);
                    return true;
                }
                default:
                    m_state = State::Invalid;
                    return std::unexpected(Http2ErrorCode::InternalError);
            }
        }

        bool isInvalid() const { return m_state == State::Invalid; }

    private:
        enum class State {
            Invalid,
            Connecting,
            Handshaking,
            SendingPreface,
            SendingSettings,
            ReceivingSettings,
            SendingSettingsAck,
            Done
        };

        H2Client& m_client;
        std::string m_host;
        uint16_t m_port;
        State m_state = State::Invalid;

        std::optional<galay::ssl::ConnectAwaitable> m_connect_awaitable;
        std::optional<SslHandshakeAwaitable> m_handshake_awaitable;
        std::optional<Http2WriteFrameAwaitableImpl<SslSocket>> m_write_awaitable;
        std::optional<Http2ReadFrameAwaitableImpl<SslSocket>> m_read_awaitable;

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
            , m_state(State::Invalid)
            , m_stream_id(0)
        {
        }

        bool await_ready() const noexcept { return false; }

        template<typename Handle>
        auto await_suspend(Handle handle) {
            switch (m_state) {
                case State::Invalid: {
                    m_state = State::SendingHeaders;
                    m_stream_id = m_client.m_next_stream_id;
                    m_client.m_next_stream_id += 2;
                    m_client.m_conn->createStream(m_stream_id);

                    std::vector<Http2HeaderField> headers;
                    headers.push_back({":method", m_request.method});
                    headers.push_back({":scheme", m_request.scheme});
                    headers.push_back({":authority", m_request.authority});
                    headers.push_back({":path", m_request.path.empty() ? "/" : m_request.path});
                    for (const auto& h : m_request.headers) headers.push_back(h);

                    bool end_stream = m_request.body.empty();
                    m_write_awaitable.emplace(m_client.m_conn->sendHeaders(m_stream_id, headers, end_stream, true));
                    return m_write_awaitable->await_suspend(handle);
                }
                case State::SendingHeaders:
                    return m_write_awaitable->await_suspend(handle);
                case State::SendingData:
                    if (!m_write_awaitable.has_value()) {
                        m_write_awaitable.emplace(m_client.m_conn->sendDataFrame(m_stream_id, m_request.body, true));
                    }
                    return m_write_awaitable->await_suspend(handle);
                case State::ReceivingResponse:
                    if (!m_read_awaitable.has_value()) {
                        m_read_awaitable.emplace(m_client.m_conn->readFrame());
                    }
                    return m_read_awaitable->await_suspend(handle);
                default:
                    return false;
            }
        }

        std::expected<std::optional<Http2Response>, Http2ErrorCode> await_resume() {
            if (m_result.has_value() && !m_result->has_value()) {
                m_state = State::Invalid;
                return std::unexpected(Http2ErrorCode::InternalError);
            }

            switch (m_state) {
                case State::SendingHeaders: {
                    auto result = m_write_awaitable->await_resume();
                    if (!result) {
                        m_write_awaitable.reset();
                        m_state = State::Invalid;
                        return std::unexpected(Http2ErrorCode::InternalError);
                    }
                    if (!result.value()) return std::nullopt;
                    m_write_awaitable.reset();

                    if (m_request.body.empty()) {
                        m_state = State::ReceivingResponse;
                    } else {
                        m_state = State::SendingData;
                    }
                    return std::nullopt;
                }
                case State::SendingData: {
                    auto result = m_write_awaitable->await_resume();
                    if (!result) {
                        m_write_awaitable.reset();
                        m_state = State::Invalid;
                        return std::unexpected(Http2ErrorCode::InternalError);
                    }
                    if (!result.value()) return std::nullopt;
                    m_write_awaitable.reset();
                    m_state = State::ReceivingResponse;
                    return std::nullopt;
                }
                case State::ReceivingResponse: {
                    auto result = m_read_awaitable->await_resume();
                    m_read_awaitable.reset();
                    if (!result) {
                        if (result.error() == Http2ErrorCode::NoError) return std::nullopt;
                        m_state = State::Invalid;
                        return std::unexpected(result.error());
                    }

                    auto& frame = *result;
                    auto stream = m_client.m_conn->getStream(m_stream_id);

                    switch (frame->type()) {
                        case Http2FrameType::Settings: {
                            auto* settings = static_cast<Http2SettingsFrame*>(frame.get());
                            if (!settings->isAck()) {
                                m_client.m_conn->peerSettings().applySettings(*settings);
                            }
                            return std::nullopt;
                        }
                        case Http2FrameType::Headers: {
                            auto* hdrs = static_cast<Http2HeadersFrame*>(frame.get());
                            if (frame->streamId() != m_stream_id) return std::nullopt;

                            if (stream) {
                                stream->appendHeaderBlock(hdrs->headerBlock());
                                if (hdrs->isEndHeaders()) {
                                    auto decode_result = m_client.m_conn->decoder().decode(stream->headerBlock());
                                    if (!decode_result) {
                                        m_state = State::Invalid;
                                        return std::unexpected(Http2ErrorCode::CompressionError);
                                    }
                                    stream->clearHeaderBlock();

                                    for (const auto& field : *decode_result) {
                                        if (field.name == ":status") {
                                            m_response.status = std::stoi(field.value);
                                        } else {
                                            m_response.headers.push_back(field);
                                        }
                                    }

                                    if (hdrs->isEndStream()) {
                                        auto resp = std::move(m_response);
                                        m_response = Http2Response();
                                        m_state = State::Invalid;
                                        return resp;
                                    }
                                }
                            }
                            return std::nullopt;
                        }
                        case Http2FrameType::Data: {
                            auto* data = static_cast<Http2DataFrame*>(frame.get());
                            if (frame->streamId() != m_stream_id) return std::nullopt;

                            m_response.body.append(data->data());

                            if (data->isEndStream()) {
                                auto resp = std::move(m_response);
                                m_response = Http2Response();
                                m_state = State::Invalid;
                                return resp;
                            }
                            return std::nullopt;
                        }
                        case Http2FrameType::WindowUpdate:
                            return std::nullopt;
                        case Http2FrameType::GoAway:
                            m_state = State::Invalid;
                            return std::unexpected(Http2ErrorCode::ProtocolError);
                        case Http2FrameType::RstStream:
                            if (frame->streamId() == m_stream_id) {
                                m_state = State::Invalid;
                                return std::unexpected(Http2ErrorCode::StreamClosed);
                            }
                            return std::nullopt;
                        default:
                            return std::nullopt;
                    }
                }
                default:
                    m_state = State::Invalid;
                    return std::unexpected(Http2ErrorCode::InternalError);
            }
        }

        bool isInvalid() const { return m_state == State::Invalid; }

    private:
        enum class State {
            Invalid,
            SendingHeaders,
            SendingData,
            ReceivingResponse
        };

        H2Client& m_client;
        Http2Request m_request;
        Http2Response m_response;
        State m_state = State::Invalid;
        uint32_t m_stream_id = 0;

        std::optional<Http2WriteFrameAwaitableImpl<SslSocket>> m_write_awaitable;
        std::optional<Http2ReadFrameAwaitableImpl<SslSocket>> m_read_awaitable;

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
