#ifndef GALAY_H2_CLIENT_H
#define GALAY_H2_CLIENT_H

#include "Http2Conn.h"
#include "Http2Stream.h"
#include "galay-http/kernel/IoVecUtils.h"
#include "galay-http/protoc/http2/Http2Base.h"
#include "galay-http/protoc/http2/Http2Frame.h"
#include "galay-http/kernel/http/HttpLog.h"
#include "galay-kernel/kernel/Awaitable.h"
#include "galay-kernel/kernel/Timeout.hpp"
#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-http/kernel/SslRecvCompatAwaitable.h"
#include "galay-ssl/async/SslSocket.h"
#include "galay-ssl/ssl/SslContext.h"
#endif
#include <memory>
#include <optional>
#include <array>
#include <cstring>
#include <vector>

namespace galay::http2
{

#ifdef GALAY_HTTP_SSL_ENABLED
using namespace galay::kernel;
using namespace galay::ssl;

namespace detail
{
inline bool parseFrameFromRingBuffer(RingBuffer& ring_buffer,
                                     uint32_t max_frame_size,
                                     std::optional<Http2ErrorCode>& error,
                                     std::vector<uint8_t>& scratch,
                                     Http2Frame::uptr& frame)
{
    std::array<iovec, 2> read_iovecs{};
    const size_t iov_count = ring_buffer.getReadIovecs(read_iovecs.data(), read_iovecs.size());
    if (iov_count == 0) {
        return false;
    }

    const size_t available = IoVecBytes::sum(read_iovecs, iov_count);
    if (available < kHttp2FrameHeaderLength) {
        return false;
    }

    uint8_t header_buf[kHttp2FrameHeaderLength];
    if (IoVecBytes::copyPrefix(read_iovecs, iov_count, header_buf, kHttp2FrameHeaderLength)
        < kHttp2FrameHeaderLength) {
        return false;
    }

    const Http2FrameHeader header = Http2FrameHeader::deserialize(header_buf);
    if (header.length > max_frame_size) {
        error = Http2ErrorCode::FrameSizeError;
        return false;
    }

    const size_t frame_size = kHttp2FrameHeaderLength + static_cast<size_t>(header.length);
    if (available < frame_size) {
        return false;
    }

    const iovec* first_segment = IoVecWindow::firstNonEmpty(read_iovecs, iov_count);
    if (first_segment != nullptr && first_segment->iov_len >= frame_size) {
        const auto* frame_data = static_cast<const uint8_t*>(first_segment->iov_base);
        auto frame_result = Http2FrameParser::parseFrame(frame_data, frame_size);
        ring_buffer.consume(frame_size);
        if (!frame_result.has_value()) {
            error = Http2ErrorCode::ProtocolError;
            return false;
        }
        frame = std::move(frame_result.value());
        return true;
    }

    if (scratch.size() < frame_size) {
        scratch.resize(frame_size);
    }

    if (IoVecBytes::copyPrefix(read_iovecs, iov_count, scratch.data(), frame_size) < frame_size) {
        return false;
    }

    auto frame_result = Http2FrameParser::parseFrame(scratch.data(), frame_size);
    ring_buffer.consume(frame_size);
    if (!frame_result.has_value()) {
        error = Http2ErrorCode::ProtocolError;
        return false;
    }
    frame = std::move(frame_result.value());
    return true;
}
} // namespace detail

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

class H2Client;

class H2ClientBuilder {
public:
    H2ClientBuilder& maxConcurrentStreams(uint32_t v)  { m_config.max_concurrent_streams = v; return *this; }
    H2ClientBuilder& initialWindowSize(uint32_t v)    { m_config.initial_window_size = v; return *this; }
    H2ClientBuilder& maxFrameSize(uint32_t v)         { m_config.max_frame_size = v; return *this; }
    H2ClientBuilder& maxHeaderListSize(uint32_t v)    { m_config.max_header_list_size = v; return *this; }
    H2ClientBuilder& verifyPeer(bool v)               { m_config.verify_peer = v; return *this; }
    H2ClientBuilder& caPath(std::string v)            { m_config.ca_path = std::move(v); return *this; }
    H2Client build() const;
    H2ClientConfig buildConfig() const                { return m_config; }
private:
    H2ClientConfig m_config;
};

/**
 * @brief H2 客户端 (HTTP/2 over TLS)
 */
class H2Client
{
public:
    class RequestAwaitable
        : public CustomAwaitable
        , public TimeoutSupport<RequestAwaitable>
    {
    public:
        using SocketType = galay::ssl::SslSocket;
        using SendAwaitableType = decltype(std::declval<SocketType>().send(
            std::declval<const char*>(), std::declval<size_t>()));
        using RecvAwaitableType = galay::http::SslRecvCompatAwaitable;

        class ProtocolSendAwaitable : public SendAwaitableType
        {
        public:
            explicit ProtocolSendAwaitable(RequestAwaitable* owner)
                : SendAwaitableType(owner->m_client.m_conn->socket().send(
                    owner->m_send_buffer.data(), owner->m_send_buffer.size()))
                , m_owner(owner)
            {
            }

#ifdef USE_IOURING
            bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override {
                if (m_owner->m_error.has_value() || m_owner->m_done) {
                    return true;
                }
                if (cqe == nullptr) {
                    return false;
                }

                const bool done = SendAwaitableType::handleComplete(cqe, handle);
                if (!done) {
                    return false;
                }

                auto send_result = std::move(this->m_sslResult);
                if (!send_result.has_value()) {
                    m_owner->setSslSendError(send_result.error());
                    return true;
                }

                return onSendCompleted(send_result.value());
            }
#else
            bool handleComplete(GHandle handle) override {
                if (m_owner->m_error.has_value() || m_owner->m_done) {
                    return true;
                }

                const bool done = SendAwaitableType::handleComplete(handle);
                if (!done) {
                    return false;
                }

                auto send_result = std::move(this->m_sslResult);
                if (!send_result.has_value()) {
                    m_owner->setSslSendError(send_result.error());
                    return true;
                }

                return onSendCompleted(send_result.value());
            }
#endif

        private:
            bool onSendCompleted(size_t sent) {
                if (sent != m_owner->m_send_buffer.size()) {
                    m_owner->m_error = Http2ErrorCode::InternalError;
                }
                return true;
            }

            RequestAwaitable* m_owner;
        };

        class ProtocolRecvAwaitable : public RecvAwaitableType
        {
        public:
            explicit ProtocolRecvAwaitable(RequestAwaitable* owner)
                : RecvAwaitableType(owner->m_client.m_conn->socket().recv(
                    owner->m_dummy_recv_buffer, sizeof(owner->m_dummy_recv_buffer)))
                , m_owner(owner)
            {
            }

#ifdef USE_IOURING
            bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override {
                if (m_owner->m_error.has_value() || m_owner->m_done) {
                    return true;
                }

                if (m_owner->consumeResponseFrames()) {
                    return true;
                }

                if (!prepareRecvWindow()) {
                    m_owner->m_error = Http2ErrorCode::InternalError;
                    return true;
                }

                if (cqe == nullptr) {
                    return false;
                }

                this->m_sslResultSet = false;
                const bool done = RecvAwaitableType::handleComplete(cqe, handle);
                if (!done) {
                    return false;
                }

                auto recv_result = std::move(this->m_sslResult);
                this->m_sslResultSet = false;
                if (!recv_result.has_value()) {
                    m_owner->setSslRecvError(recv_result.error());
                    return true;
                }

                const size_t bytes_read = recv_result.value().size();
                if (bytes_read == 0) {
                    m_owner->m_error = Http2ErrorCode::ConnectError;
                    return true;
                }

                m_owner->m_client.m_conn->ringBuffer().produce(bytes_read);
                return m_owner->consumeResponseFrames();
            }
#else
            bool handleComplete(GHandle handle) override {
                while (true) {
                    if (m_owner->m_error.has_value() || m_owner->m_done) {
                        return true;
                    }

                    if (m_owner->consumeResponseFrames()) {
                        return true;
                    }

                    if (!prepareRecvWindow()) {
                        m_owner->m_error = Http2ErrorCode::InternalError;
                        return true;
                    }

                    this->m_sslResultSet = false;
                    const bool done = RecvAwaitableType::handleComplete(handle);
                    if (!done) {
                        return false;
                    }

                    auto recv_result = std::move(this->m_sslResult);
                    this->m_sslResultSet = false;
                    if (!recv_result.has_value()) {
                        m_owner->setSslRecvError(recv_result.error());
                        return true;
                    }

                    const size_t bytes_read = recv_result.value().size();
                    if (bytes_read == 0) {
                        m_owner->m_error = Http2ErrorCode::ConnectError;
                        return true;
                    }

                    m_owner->m_client.m_conn->ringBuffer().produce(bytes_read);
                }
            }
#endif

        private:
            bool prepareRecvWindow() {
                auto write_iovecs = borrowWriteIovecs(m_owner->m_client.m_conn->ringBuffer());
                return IoVecWindow::bindFirstNonEmpty(write_iovecs, this->m_plainBuffer, this->m_plainLength);
            }

            RequestAwaitable* m_owner;
        };

        RequestAwaitable(H2Client& client, Http2Request&& request)
            : CustomAwaitable(client.m_conn
                              ? client.m_conn->socket().controller()
                              : client.m_dummy_socket.controller())
            , m_client(client)
            , m_request(std::move(request))
            , m_recv_awaitable(this)
            , m_result(true)
        {
            if (!m_client.m_connected || !m_client.m_conn) {
                m_error = Http2ErrorCode::ConnectError;
                m_done = true;
                return;
            }

            if (m_client.m_conn->isDraining() || m_client.m_conn->isGoawayReceived()) {
                m_error = Http2ErrorCode::RefusedStream;
                m_done = true;
                return;
            }

            m_stream_id = m_client.m_next_stream_id;
            m_client.m_next_stream_id += 2;
            m_client.m_conn->createStream(m_stream_id);

            std::vector<Http2HeaderField> headers;
            headers.push_back({":method", m_request.method});
            headers.push_back({":scheme", m_request.scheme});
            headers.push_back({":authority", m_request.authority});
            headers.push_back({":path", m_request.path.empty() ? "/" : m_request.path});
            for (const auto& h : m_request.headers) {
                headers.push_back(h);
            }

            auto headers_frame = Http2FrameBuilder::headers(m_stream_id,
                                                            m_client.m_conn->encoder().encode(headers),
                                                            m_request.body.empty(),
                                                            true);
            m_send_buffer = headers_frame->serialize();

            if (!m_request.body.empty()) {
                auto data_frame = Http2FrameBuilder::data(m_stream_id, std::string(m_request.body), true);
                m_send_buffer.append(data_frame->serialize());
            }

            m_send_awaitable.emplace(this);

            addTask(IOEventType::SEND, &*m_send_awaitable);
            addTask(IOEventType::RECV, &m_recv_awaitable);
        }

        bool await_ready() const noexcept { return m_done; }
        using CustomAwaitable::await_suspend;

        std::expected<std::optional<Http2Response>, Http2ErrorCode> await_resume() {
            if (m_done && m_error.has_value()) {
                return std::unexpected(*m_error);
            }

            onCompleted();

            if (!m_result.has_value()) {
                return std::unexpected(Http2ErrorCode::InternalError);
            }

            if (m_error.has_value()) {
                return std::unexpected(*m_error);
            }

            if (!m_done || !m_response.has_value()) {
                return std::nullopt;
            }

            return std::move(*m_response);
        }

        bool isInvalid() const { return m_done || m_error.has_value(); }

    private:
        friend class ProtocolSendAwaitable;
        friend class ProtocolRecvAwaitable;

        void setSslSendError(const galay::ssl::SslError& error) {
            HTTP_LOG_ERROR("[h2] [send-fail] [{}]", error.message());
            m_error = Http2ErrorCode::InternalError;
        }

        void setSslRecvError(const galay::ssl::SslError& error) {
            HTTP_LOG_ERROR("[h2] [recv-fail] [{}]", error.message());
            m_error = Http2ErrorCode::InternalError;
        }

        bool parseNextFrame(Http2Frame::uptr& frame) {
            return detail::parseFrameFromRingBuffer(
                m_client.m_conn->ringBuffer(),
                m_client.m_conn->peerSettings().max_frame_size,
                m_error,
                m_frame_scratch,
                frame);
        }

        bool decodeHeaderBlock() {
            auto decode_result = m_client.m_conn->decoder().decode(m_header_block);
            if (!decode_result.has_value()) {
                m_error = Http2ErrorCode::CompressionError;
                return false;
            }

            for (const auto& field : decode_result.value()) {
                if (field.name == ":status") {
                    try {
                        m_response_data.status = std::stoi(field.value);
                    } catch (...) {
                        m_error = Http2ErrorCode::ProtocolError;
                        return false;
                    }
                } else {
                    m_response_data.headers.push_back(field);
                }
            }

            m_header_block.clear();
            return true;
        }

        bool consumeResponseFrames() {
            if (m_error.has_value() || m_done) {
                return true;
            }

            while (true) {
                Http2Frame::uptr frame;
                if (!parseNextFrame(frame)) {
                    return m_error.has_value() || m_done;
                }

                if (!frame) {
                    continue;
                }

                switch (frame->type()) {
                    case Http2FrameType::Settings: {
                        auto* settings = static_cast<Http2SettingsFrame*>(frame.get());
                        if (settings->isAck()) {
                            m_client.m_conn->markSettingsAckReceived();
                        } else {
                            auto err = m_client.m_conn->peerSettings().applySettings(*settings);
                            if (err != Http2ErrorCode::NoError) {
                                m_error = err;
                                return true;
                            }
                        }
                        continue;
                    }
                    case Http2FrameType::Headers: {
                        auto* hdrs = static_cast<Http2HeadersFrame*>(frame.get());
                        if (frame->streamId() != m_stream_id) {
                            continue;
                        }

                        m_header_block.append(hdrs->headerBlock());
                        if (hdrs->isEndHeaders()) {
                            if (!decodeHeaderBlock()) {
                                return true;
                            }
                            if (hdrs->isEndStream()) {
                                m_response = std::move(m_response_data);
                                m_done = true;
                                return true;
                            }
                        }
                        continue;
                    }
                    case Http2FrameType::Continuation: {
                        auto* cont = static_cast<Http2ContinuationFrame*>(frame.get());
                        if (frame->streamId() != m_stream_id) {
                            continue;
                        }

                        m_header_block.append(cont->headerBlock());
                        if (cont->isEndHeaders()) {
                            if (!decodeHeaderBlock()) {
                                return true;
                            }
                        }
                        continue;
                    }
                    case Http2FrameType::Data: {
                        auto* data = static_cast<Http2DataFrame*>(frame.get());
                        if (frame->streamId() != m_stream_id) {
                            continue;
                        }

                        m_response_data.body.append(data->data());
                        if (data->isEndStream()) {
                            m_response = std::move(m_response_data);
                            m_done = true;
                            return true;
                        }
                        continue;
                    }
                    case Http2FrameType::GoAway:
                        if (auto* goaway = frame->asGoAway()) {
                            m_client.m_conn->markGoawayReceived(
                                goaway->lastStreamId(), goaway->errorCode(), goaway->debugData());
                            m_error = (m_stream_id > goaway->lastStreamId())
                                ? Http2ErrorCode::RefusedStream
                                : Http2ErrorCode::ProtocolError;
                        } else {
                            m_error = Http2ErrorCode::ProtocolError;
                        }
                        m_done = true;
                        return true;
                    case Http2FrameType::RstStream:
                        if (frame->streamId() == m_stream_id) {
                            m_error = Http2ErrorCode::StreamClosed;
                            m_done = true;
                            return true;
                        }
                        continue;
                    default:
                        continue;
                }
            }
        }

        H2Client& m_client;
        Http2Request m_request;
        std::string m_send_buffer;
        std::string m_header_block;
        Http2Response m_response_data;
        std::optional<Http2Response> m_response;
        uint32_t m_stream_id = 0;
        std::optional<ProtocolSendAwaitable> m_send_awaitable;
        ProtocolRecvAwaitable m_recv_awaitable;
        std::optional<Http2ErrorCode> m_error;
        bool m_done = false;
        char m_dummy_recv_buffer[1]{0};
        std::vector<uint8_t> m_frame_scratch;

    public:
        std::expected<bool, IOError> m_result;
    };

    class ConnectAwaitable : public TimeoutSupport<ConnectAwaitable>
    {
    public:
        ConnectAwaitable(H2Client& client, std::string host, uint16_t port)
            : m_client(client)
            , m_host(std::move(host))
            , m_port(port)
        {
        }

        bool await_ready() const noexcept { return false; }

        template<typename Handle>
        bool await_suspend(Handle handle) {
            if (!m_started) {
                m_started = true;
                m_wait_result.emplace(m_client.connectImpl(m_host, m_port).wait());
            }
            return m_wait_result->await_suspend(handle);
        }

        ConnectAwaitable& wait() & { return *this; }
        ConnectAwaitable&& wait() && { return std::move(*this); }

        std::expected<bool, Http2ErrorCode> await_resume() {
            return m_client.m_connect_result;
        }

    private:
        H2Client& m_client;
        std::string m_host;
        uint16_t m_port;
        bool m_started = false;
        std::optional<WaitResult> m_wait_result;
    };

    class CloseAwaitable : public TimeoutSupport<CloseAwaitable>
    {
    public:
        using ManagerShutdownAwaitable = Http2StreamManagerImpl<SslSocket>::ShutdownAwaitable;

        explicit CloseAwaitable(H2Client& client)
            : m_client(client)
        {
        }

        bool await_ready() const noexcept { return false; }

        template<typename Handle>
        bool await_suspend(Handle handle) {
            if (m_started) {
                return false;
            }
            m_started = true;
            m_client.m_connected = false;
            m_client.m_close_result = true;

            if (m_client.m_conn) {
                if (auto* mgr = m_client.m_conn->streamManager()) {
                    m_manager_shutdown.emplace(mgr->shutdown(Http2ErrorCode::NoError));
                    return m_manager_shutdown->await_suspend(handle);
                }

                m_socket_close.emplace(m_client.m_conn->close());
                return m_socket_close->await_suspend(handle);
            }

            if (m_client.m_socket) {
                m_socket_close.emplace(m_client.m_socket->close());
                return m_socket_close->await_suspend(handle);
            }

            return false;
        }

        CloseAwaitable& wait() & { return *this; }
        CloseAwaitable&& wait() && { return std::move(*this); }

        std::expected<bool, Http2ErrorCode> await_resume() {
            if (m_socket_close.has_value()) {
                (void)m_socket_close->await_resume();
            }

            m_client.m_conn.reset();
            m_client.m_socket.reset();
            return m_client.m_close_result;
        }

    private:
        H2Client& m_client;
        bool m_started = false;
        std::optional<ManagerShutdownAwaitable> m_manager_shutdown;
        std::optional<galay::kernel::CloseAwaitable> m_socket_close;
    };

public:
    H2Client(const H2ClientConfig& config = H2ClientConfig())
        : m_config(config)
        , m_connected(false)
        , m_next_stream_id(1)
        , m_ssl_ctx(SslMethod::TLS_Client)
    {
        m_ssl_ctx.setALPNProtocols({"h2"});

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
    H2Client(H2Client&&) noexcept = default;
    H2Client& operator=(H2Client&&) noexcept = default;

    ConnectAwaitable connect(const std::string& host, uint16_t port = 443) {
        return ConnectAwaitable(*this, host, port);
    }

    Http2Stream::ptr get(const std::string& path) {
        if (!m_connected || !m_conn || !m_conn->streamManager()) {
            HTTP_LOG_ERROR("[h2] [get] [not-ready]");
            return nullptr;
        }

        auto* mgr = m_conn->streamManager();
        auto stream = mgr->allocateStream();
        std::vector<Http2HeaderField> headers;
        headers.reserve(4);
        headers.emplace_back(":method", "GET");
        headers.emplace_back(":scheme", "https");
        headers.emplace_back(":authority", m_authority);
        headers.emplace_back(":path", path.empty() ? "/" : path);
        stream->sendHeaders(headers, true);
        return stream;
    }

    Http2Stream::ptr post(const std::string& path,
                          const std::string& body,
                          const std::string& content_type = "application/x-www-form-urlencoded") {
        if (!m_connected || !m_conn || !m_conn->streamManager()) {
            HTTP_LOG_ERROR("[h2] [post] [not-ready]");
            return nullptr;
        }

        auto* mgr = m_conn->streamManager();
        auto stream = mgr->allocateStream();
        std::vector<Http2HeaderField> headers;
        headers.reserve(5);
        headers.emplace_back(":method", "POST");
        headers.emplace_back(":scheme", "https");
        headers.emplace_back(":authority", m_authority);
        headers.emplace_back(":path", path.empty() ? "/" : path);
        headers.emplace_back("content-type", content_type);
        stream->sendHeaders(headers, false);
        stream->sendData(body, true);
        return stream;
    }

    CloseAwaitable close() {
        return CloseAwaitable(*this);
    }

    bool isConnected() const { return m_connected; }

    std::string getALPNProtocol() const {
        if (m_socket) {
            return m_socket->getALPNProtocol();
        }
        return m_alpn_protocol;
    }

private:
    Coroutine connectImpl(std::string host, uint16_t port);
    H2ClientConfig m_config;
    std::string m_host;
    std::string m_authority;
    uint16_t m_port = 0;
    bool m_connected = false;
    uint32_t m_next_stream_id = 1;
    std::string m_alpn_protocol;

    SslContext m_ssl_ctx;
    SslSocket m_dummy_socket{nullptr};
    std::unique_ptr<SslSocket> m_socket;
    std::unique_ptr<Http2ConnImpl<SslSocket>> m_conn;
    std::expected<bool, Http2ErrorCode> m_connect_result{true};
    std::expected<bool, Http2ErrorCode> m_close_result{true};
};

inline Coroutine H2Client::connectImpl(std::string host, uint16_t port) {
    m_connected = false;
    m_host = std::move(host);
    m_port = port;
    m_authority = m_host + ":" + std::to_string(m_port);
    m_alpn_protocol.clear();
    m_conn.reset();
    m_connect_result = std::unexpected(Http2ErrorCode::ConnectError);

    m_socket = std::make_unique<SslSocket>(&m_ssl_ctx);
    auto nonblock_result = m_socket->option().handleNonBlock();
    if (!nonblock_result) {
        HTTP_LOG_ERROR("[h2] [connect] [nonblock-fail] [{}]", nonblock_result.error().message());
        co_await m_socket->close();
        m_socket.reset();
        co_return;
    }

    (void)m_socket->setHostname(m_host);

    Host server_host(IPType::IPV4, m_host, m_port);
    auto connect_result = co_await m_socket->connect(server_host);
    if (!connect_result) {
        HTTP_LOG_ERROR("[h2] [connect-fail] [{}]", connect_result.error().message());
        co_await m_socket->close();
        m_socket.reset();
        co_return;
    }

    auto handshake_result = co_await m_socket->handshake();
    if (!handshake_result) {
        HTTP_LOG_ERROR("[h2] [handshake-fail] [{}]", handshake_result.error().message());
        co_await m_socket->close();
        m_socket.reset();
        co_return;
    }

    m_alpn_protocol = m_socket->getALPNProtocol();
    if (m_alpn_protocol != "h2") {
        HTTP_LOG_ERROR("[h2] [alpn-fail] [got={}] [expect=h2]", m_alpn_protocol);
        co_await m_socket->close();
        m_socket.reset();
        co_return;
    }

    m_conn = std::make_unique<Http2ConnImpl<SslSocket>>(std::move(*m_socket));
    m_socket.reset();
    m_conn->setIsClient(true);

    m_conn->localSettings().max_concurrent_streams = m_config.max_concurrent_streams;
    m_conn->localSettings().initial_window_size = m_config.initial_window_size;
    m_conn->localSettings().max_frame_size = m_config.max_frame_size;
    m_conn->localSettings().max_header_list_size = m_config.max_header_list_size;
    m_conn->localSettings().enable_push = 0;

    std::string preface(kHttp2ConnectionPreface.begin(), kHttp2ConnectionPreface.end());
    Http2SettingsFrame settings = m_conn->localSettings().toFrame();
    settings.header().stream_id = 0;
    preface.append(settings.serialize());

    auto send_preface_result = co_await m_conn->writeRaw(std::move(preface));
    if (!send_preface_result) {
        HTTP_LOG_ERROR("[h2] [preface-send-fail] [code={}]",
                       http2ErrorCodeToString(send_preface_result.error()));
        co_await m_conn->close();
        m_conn.reset();
        co_return;
    }

    m_conn->markSettingsSent();
    m_conn->initStreamManager();
    co_await m_conn->streamManager()->startInBackground(
        [](Http2Stream::ptr) -> Coroutine { co_return; });

    m_connected = true;
    m_connect_result = true;
    HTTP_LOG_INFO("[connect] [h2] [{}:{}]", m_host, m_port);
    co_return;
}

inline H2Client H2ClientBuilder::build() const { return H2Client(m_config); }

#endif // GALAY_HTTP_SSL_ENABLED

} // namespace galay::http2

#endif // GALAY_H2_CLIENT_H
