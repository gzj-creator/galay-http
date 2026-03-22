#ifndef GALAY_H2_CLIENT_H
#define GALAY_H2_CLIENT_H

#include "Http2Conn.h"
#include "Http2Stream.h"
#include "galay-http/kernel/IoVecUtils.h"
#include "galay-http/kernel/http/HttpLog.h"
#include "galay-http/protoc/http2/Http2Base.h"
#include "galay-http/protoc/http2/Http2Frame.h"
#include "galay-kernel/kernel/Awaitable.h"
#include "galay-kernel/kernel/Task.h"
#include "galay-kernel/kernel/Timeout.hpp"
#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/async/SslAwaitableCore.h"
#include "galay-ssl/async/SslSocket.h"
#include "galay-ssl/ssl/SslContext.h"
#endif
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

namespace galay::http2
{

#ifdef GALAY_HTTP_SSL_ENABLED
using namespace galay::kernel;

struct H2ClientConfig
{
    uint32_t max_concurrent_streams = 100;
    uint32_t initial_window_size = 65535;
    uint32_t max_frame_size = 16384;
    uint32_t max_header_list_size = 8192;
    bool verify_peer = false;
    std::string ca_path;
};

class H2Client;

namespace detail
{
inline bool parseFrameFromRingBuffer(RingBuffer& ring_buffer,
                                     uint32_t max_frame_size,
                                     std::optional<Http2ErrorCode>& error,
                                     std::vector<uint8_t>& scratch,
                                     Http2Frame::uptr& frame);

struct H2ClientConnectMachine;
class H2ClientConnectSequence;
struct H2RequestMachine;
auto buildRequestOperation(H2Client& client, Http2Request&& request);
class H2ClientCloseAwaitable;
} // namespace detail

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

class H2Client
{
public:
    using ConnectAwaitable = detail::H2ClientConnectSequence;
    using CloseAwaitable = detail::H2ClientCloseAwaitable;

    H2Client(const H2ClientConfig& config = H2ClientConfig());
    ~H2Client() = default;

    H2Client(const H2Client&) = delete;
    H2Client& operator=(const H2Client&) = delete;
    H2Client(H2Client&&) noexcept = default;
    H2Client& operator=(H2Client&&) noexcept = default;

    auto connect(const std::string& host, uint16_t port = 443);
    auto request(Http2Request request);

    Http2Stream::ptr get(const std::string& path);
    Http2Stream::ptr post(const std::string& path,
                          const std::string& body,
                          const std::string& content_type = "application/x-www-form-urlencoded");
    auto close();

    bool isConnected() const { return m_connected; }

    std::string getALPNProtocol() const {
        if (m_socket) {
            return m_socket->getALPNProtocol();
        }
        return m_alpn_protocol;
    }

private:
    friend struct detail::H2ClientConnectMachine;
    friend class detail::H2ClientConnectSequence;
    friend class detail::H2ClientCloseAwaitable;
    friend struct detail::H2RequestMachine;
    friend auto detail::buildRequestOperation(H2Client& client, Http2Request&& request);

    H2ClientConfig m_config;
    std::string m_host;
    std::string m_authority;
    uint16_t m_port = 0;
    bool m_connected = false;
    uint32_t m_next_stream_id = 1;
    std::string m_alpn_protocol;
    Scheduler* m_scheduler = nullptr;

    galay::ssl::SslContext m_ssl_ctx;
    galay::ssl::SslSocket m_dummy_socket{nullptr};
    std::unique_ptr<galay::ssl::SslSocket> m_socket;
    std::unique_ptr<Http2ConnImpl<galay::ssl::SslSocket>> m_conn;
    std::expected<bool, Http2ErrorCode> m_connect_result{true};
    std::expected<bool, Http2ErrorCode> m_close_result{true};

    bool ensureActiveStreamManager();
};

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

struct H2ClientConnectMachine {
    using result_type = std::expected<bool, Http2ErrorCode>;

    explicit H2ClientConnectMachine(H2Client& client)
        : m_client(&client)
        , m_server_host(IPType::IPV4, client.m_host, client.m_port)
        , m_driver(client.m_socket.get())
    {
    }

    MachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        switch (m_phase) {
            case Phase::kConnect:
                return MachineAction<result_type>::waitConnect(m_server_host);
            case Phase::kHandshake:
            case Phase::kSendPreface:
                return advanceSsl();
            case Phase::kDone:
                return MachineAction<result_type>::complete(
                    m_result.value_or(std::unexpected(Http2ErrorCode::ConnectError)));
        }

        fail("internal-fail", "invalid connect phase");
        return MachineAction<result_type>::complete(std::move(*m_result));
    }

    void onConnect(std::expected<void, IOError> result) {
        if (!result) {
            HTTP_LOG_ERROR("[h2] [connect-fail] [{}]", result.error().message());
            fail();
            return;
        }

        m_phase = Phase::kHandshake;
        m_driver.startHandshake();
    }

    void onRead(std::expected<size_t, IOError> result) {
        m_driver.onRead(std::move(result));
    }

    void onWrite(std::expected<size_t, IOError> result) {
        m_driver.onWrite(std::move(result));
    }

private:
    enum class Phase : uint8_t {
        kConnect,
        kHandshake,
        kSendPreface,
        kDone,
    };

    MachineAction<result_type> advanceSsl() {
        auto wait = m_driver.poll();
        if (m_driver.completed()) {
            if (m_phase == Phase::kHandshake) {
                handleHandshakeResult(m_driver.takeHandshakeResult());
            } else if (m_phase == Phase::kSendPreface) {
                handlePrefaceSendResult(m_driver.takeSendResult());
            } else {
                fail("internal-fail", "ssl driver completed in invalid phase");
            }
            return advance();
        }

        if (wait.kind == galay::ssl::SslOperationDriver::WaitKind::kRead) {
            return MachineAction<result_type>::waitRead(
                m_driver.recvContext().m_buffer,
                m_driver.recvContext().m_length);
        }

        if (wait.kind == galay::ssl::SslOperationDriver::WaitKind::kWrite) {
            return MachineAction<result_type>::waitWrite(
                m_driver.sendContext().m_buffer,
                m_driver.sendContext().m_length);
        }

        fail("internal-fail", "ssl driver returned no wait action");
        return MachineAction<result_type>::complete(std::move(*m_result));
    }

    void handleHandshakeResult(std::expected<void, galay::ssl::SslError> result) {
        if (!result) {
            HTTP_LOG_ERROR("[h2] [handshake-fail] [{}]", result.error().message());
            fail();
            return;
        }

        m_client->m_alpn_protocol = m_client->m_socket->getALPNProtocol();
        if (m_client->m_alpn_protocol != "h2") {
            HTTP_LOG_ERROR("[h2] [alpn-fail] [got={}] [expect=h2]", m_client->m_alpn_protocol);
            fail();
            return;
        }

        preparePreface();
        m_phase = Phase::kSendPreface;
        m_driver.startSend(m_preface.data(), m_preface.size());
    }

    void handlePrefaceSendResult(std::expected<size_t, galay::ssl::SslError> result) {
        if (!result) {
            HTTP_LOG_ERROR("[h2] [preface-send-fail] [{}]", result.error().message());
            fail();
            return;
        }

        if (result.value() != m_preface.size()) {
            HTTP_LOG_ERROR("[h2] [preface-send-fail] [short-write]");
            fail();
            return;
        }

        finalizeConnection();
    }

    void preparePreface() {
        Http2Settings local_settings;
        local_settings.max_concurrent_streams = m_client->m_config.max_concurrent_streams;
        local_settings.initial_window_size = m_client->m_config.initial_window_size;
        local_settings.max_frame_size = m_client->m_config.max_frame_size;
        local_settings.max_header_list_size = m_client->m_config.max_header_list_size;
        local_settings.enable_push = 0;

        m_preface.assign(kHttp2ConnectionPreface.begin(), kHttp2ConnectionPreface.end());
        auto settings = local_settings.toFrame();
        settings.header().stream_id = 0;
        m_preface.append(settings.serialize());
    }

    void finalizeConnection() {
        m_client->m_connected = true;
        m_client->m_connect_result = true;
        m_result = true;
        m_phase = Phase::kDone;
    }

    void fail(const char* phase = nullptr, const char* detail = nullptr) {
        if (phase != nullptr && detail != nullptr) {
            HTTP_LOG_ERROR("[h2] [{}] [{}]", phase, detail);
        }
        if (m_client->m_socket && m_client->m_socket->handle().fd >= 0) {
            ::close(m_client->m_socket->handle().fd);
        }
        m_client->m_socket.reset();
        m_client->m_conn.reset();
        m_client->m_connected = false;
        m_client->m_connect_result = std::unexpected(Http2ErrorCode::ConnectError);
        m_result = std::unexpected(Http2ErrorCode::ConnectError);
        m_phase = Phase::kDone;
    }

    H2Client* m_client = nullptr;
    Host m_server_host;
    galay::ssl::SslOperationDriver m_driver;
    std::string m_preface;
    std::optional<result_type> m_result;
    Phase m_phase = Phase::kConnect;
};

class H2ClientConnectSequence
    : public SequenceAwaitableBase
    , public TimeoutSupport<H2ClientConnectSequence>
{
public:
    using ResultType = std::expected<bool, Http2ErrorCode>;
    using result_type = ResultType;

    H2ClientConnectSequence(const H2ClientConnectSequence&) = delete;
    H2ClientConnectSequence& operator=(const H2ClientConnectSequence&) = delete;
    H2ClientConnectSequence(H2ClientConnectSequence&&) noexcept = default;
    H2ClientConnectSequence& operator=(H2ClientConnectSequence&&) noexcept = default;

private:
    static void discardSocket(std::unique_ptr<galay::ssl::SslSocket>& socket) {
        if (socket && socket->handle().fd >= 0) {
            ::close(socket->handle().fd);
        }
        socket.reset();
    }

    static void discardTransport(H2Client& client) {
        if (client.m_conn) {
            if (client.m_conn->socket().handle().fd >= 0) {
                ::close(client.m_conn->socket().handle().fd);
            }
            client.m_conn.reset();
        }
        discardSocket(client.m_socket);
        client.m_connected = false;
        client.m_scheduler = nullptr;
    }

    static bool finalizeTransport(H2Client& client) {
        if (client.m_socket == nullptr) {
            return false;
        }

        client.m_conn =
            std::make_unique<Http2ConnImpl<galay::ssl::SslSocket>>(std::move(*client.m_socket));
        client.m_socket.reset();
        client.m_conn->setIsClient(true);
        client.m_conn->localSettings().max_concurrent_streams = client.m_config.max_concurrent_streams;
        client.m_conn->localSettings().initial_window_size = client.m_config.initial_window_size;
        client.m_conn->localSettings().max_frame_size = client.m_config.max_frame_size;
        client.m_conn->localSettings().max_header_list_size = client.m_config.max_header_list_size;
        client.m_conn->localSettings().enable_push = 0;
        client.m_conn->markSettingsSent();
        client.m_connected = true;
        client.m_connect_result = true;
        HTTP_LOG_INFO("[connect] [h2] [{}:{}]", client.m_host, client.m_port);
        return true;
    }

public:
    H2ClientConnectSequence(H2Client& client, std::string host, uint16_t port)
        : SequenceAwaitableBase(client.m_dummy_socket.controller())
        , m_client(&client)
        , m_result(true)
    {
        m_client->m_connected = false;
        m_client->m_host = std::move(host);
        m_client->m_port = port;
        m_client->m_authority = m_client->m_host + ":" + std::to_string(m_client->m_port);
        m_client->m_alpn_protocol.clear();
        m_client->m_scheduler = nullptr;
        m_client->m_connect_result = std::unexpected(Http2ErrorCode::ConnectError);
        m_client->m_close_result = true;
        m_client->m_conn.reset();
        m_client->m_socket = std::make_unique<galay::ssl::SslSocket>(&m_client->m_ssl_ctx);
        m_controller = m_client->m_socket->controller();

        auto nonblock_result = m_client->m_socket->option().handleNonBlock();
        if (!nonblock_result) {
            HTTP_LOG_ERROR("[h2] [connect] [nonblock-fail] [{}]", nonblock_result.error().message());
            discardTransport(*m_client);
            m_ready = true;
            return;
        }

        auto sni_result = m_client->m_socket->setHostname(m_client->m_host);
        if (!sni_result) {
            HTTP_LOG_ERROR("[h2] [sni-fail] [{}]", sni_result.error().message());
            discardTransport(*m_client);
            m_ready = true;
            return;
        }

        m_inner_operation = std::make_unique<InnerOperation>(
            AwaitableBuilder<ResultType>::fromStateMachine(
                m_client->m_socket->controller(),
                H2ClientConnectMachine(*m_client))
                .build()
        );
    }

    ~H2ClientConnectSequence() {
        cleanupInnerIfArmed();
    }

    bool await_ready() const noexcept {
        return m_ready || (m_inner_operation != nullptr && m_inner_operation->await_ready());
    }

    template<typename Promise>
    decltype(auto) await_suspend(std::coroutine_handle<Promise> handle) {
        if (m_inner_operation == nullptr) {
            return false;
        }
        m_client->m_scheduler = handle.promise().taskRefView().belongScheduler();
        m_inner_armed = true;
        return m_inner_operation->await_suspend(handle);
    }

    ResultType await_resume() {
        if (!m_result.has_value()) {
            cleanupInnerIfArmed();
            discardTransport(*m_client);
            m_client->m_connect_result = std::unexpected(Http2ErrorCode::ConnectError);
            return std::unexpected(Http2ErrorCode::ConnectError);
        }

        if (m_ready) {
            return std::unexpected(Http2ErrorCode::ConnectError);
        }

        auto result = resumeInner();
        if (result && !finalizeTransport(*m_client)) {
            discardTransport(*m_client);
            m_client->m_connect_result = std::unexpected(Http2ErrorCode::ConnectError);
            return std::unexpected(Http2ErrorCode::ConnectError);
        }
        return result;
    }

    IOTask* front() override {
        return m_inner_operation ? m_inner_operation->front() : nullptr;
    }

    const IOTask* front() const override {
        return m_inner_operation ? m_inner_operation->front() : nullptr;
    }

    void popFront() override {
        if (m_inner_operation) {
            m_inner_operation->popFront();
        }
    }

    bool empty() const override {
        return m_inner_operation == nullptr || m_inner_operation->empty();
    }

#ifdef USE_IOURING
    SequenceProgress prepareForSubmit() override {
        return m_inner_operation ? m_inner_operation->prepareForSubmit() : SequenceProgress::kCompleted;
    }

    SequenceProgress onActiveEvent(struct io_uring_cqe* cqe, GHandle handle) override {
        return m_inner_operation ? m_inner_operation->onActiveEvent(cqe, handle) : SequenceProgress::kCompleted;
    }
#else
    SequenceProgress prepareForSubmit(GHandle handle) override {
        return m_inner_operation ? m_inner_operation->prepareForSubmit(handle) : SequenceProgress::kCompleted;
    }

    SequenceProgress onActiveEvent(GHandle handle) override {
        return m_inner_operation ? m_inner_operation->onActiveEvent(handle) : SequenceProgress::kCompleted;
    }
#endif

public:
    std::expected<bool, IOError> m_result;

private:
    using InnerOperation = galay::kernel::StateMachineAwaitable<H2ClientConnectMachine>;

    ResultType resumeInner() {
        m_inner_completed = true;
        return m_inner_operation->await_resume();
    }

    void cleanupInnerIfArmed() {
        if (m_inner_operation != nullptr && m_inner_armed && !m_inner_completed) {
            m_inner_operation->onCompleted();
            m_inner_completed = true;
        }
    }

    H2Client* m_client = nullptr;
    std::unique_ptr<InnerOperation> m_inner_operation;
    bool m_ready = false;
    bool m_inner_armed = false;
    bool m_inner_completed = false;
};

struct H2RequestMachine {
    using result_type = std::expected<std::optional<Http2Response>, Http2ErrorCode>;

    H2RequestMachine(H2Client* client, Http2Request request)
        : m_client(client)
        , m_request(std::move(request))
    {
    }

    galay::ssl::SslMachineAction<result_type> advance() {
        if (!m_initialized) {
            initialize();
        }

        if (m_result.has_value()) {
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        if (m_error.has_value()) {
            m_result = std::unexpected(*m_error);
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        if (hasPendingControlSend()) {
            return galay::ssl::SslMachineAction<result_type>::send(
                m_control_send_buffer.data() + m_control_send_offset,
                m_control_send_buffer.size() - m_control_send_offset);
        }

        if (m_send_offset < m_send_buffer.size()) {
            return galay::ssl::SslMachineAction<result_type>::send(
                m_send_buffer.data() + m_send_offset,
                m_send_buffer.size() - m_send_offset);
        }

        if (consumeResponseFrames()) {
            if (m_result.has_value()) {
                return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
            }
            if (m_error.has_value()) {
                m_result = std::unexpected(*m_error);
                return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
            }
        }

        char* recv_buffer = nullptr;
        size_t recv_length = 0;
        if (!prepareRecvWindow(recv_buffer, recv_length)) {
            m_error = Http2ErrorCode::InternalError;
            m_result = std::unexpected(*m_error);
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        return galay::ssl::SslMachineAction<result_type>::recv(recv_buffer, recv_length);
    }

    void onHandshake(std::expected<void, galay::ssl::SslError>) {}

    void onRecv(std::expected<Bytes, galay::ssl::SslError> result) {
        if (!result) {
            setSslRecvError(result.error());
            m_result = std::unexpected(*m_error);
            return;
        }

        const size_t bytes_read = result.value().size();
        if (bytes_read == 0) {
            m_error = Http2ErrorCode::ConnectError;
            m_result = std::unexpected(*m_error);
            return;
        }

        m_client->m_conn->ringBuffer().produce(bytes_read);
        if (consumeResponseFrames() && !m_result.has_value() && m_error.has_value()) {
            m_result = std::unexpected(*m_error);
        }
    }

    void onSend(std::expected<size_t, galay::ssl::SslError> result) {
        if (!result) {
            setSslSendError(result.error());
            m_result = std::unexpected(*m_error);
            return;
        }

        if (result.value() == 0) {
            m_error = Http2ErrorCode::InternalError;
            m_result = std::unexpected(*m_error);
            return;
        }

        if (hasPendingControlSend()) {
            m_control_send_offset += result.value();
            if (m_control_send_offset > m_control_send_buffer.size()) {
                m_error = Http2ErrorCode::InternalError;
                m_result = std::unexpected(*m_error);
                return;
            }
            if (m_control_send_offset == m_control_send_buffer.size()) {
                m_control_send_buffer.clear();
                m_control_send_offset = 0;
            }
            return;
        }

        m_send_offset += result.value();
        if (m_send_offset > m_send_buffer.size()) {
            m_error = Http2ErrorCode::InternalError;
            m_result = std::unexpected(*m_error);
        }
    }

    void onShutdown(std::expected<void, galay::ssl::SslError>) {}

private:
    void initialize() {
        m_initialized = true;

        if (!m_client->m_connected || !m_client->m_conn) {
            m_error = Http2ErrorCode::ConnectError;
            return;
        }

        if (m_client->m_conn->isDraining() || m_client->m_conn->isGoawayReceived()) {
            m_error = Http2ErrorCode::RefusedStream;
            return;
        }

        m_stream_id = m_client->m_next_stream_id;
        m_client->m_next_stream_id += 2;
        m_client->m_conn->createStream(m_stream_id);

        std::vector<Http2HeaderField> headers;
        headers.push_back({":method", m_request.method});
        headers.push_back({":scheme", m_request.scheme});
        headers.push_back({":authority", m_request.authority});
        headers.push_back({":path", m_request.path.empty() ? "/" : m_request.path});
        for (const auto& header : m_request.headers) {
            headers.push_back(header);
        }

        auto headers_frame = Http2FrameBuilder::headers(
            m_stream_id,
            m_client->m_conn->encoder().encode(headers),
            m_request.body.empty(),
            true);
        m_send_buffer = headers_frame->serialize();

        if (!m_request.body.empty()) {
            auto data_frame = Http2FrameBuilder::data(
                m_stream_id,
                m_request.takeCoalescedBody(),
                true);
            m_send_buffer.append(data_frame->serialize());
        }
    }

    bool prepareRecvWindow(char*& buffer, size_t& length) {
        auto write_iovecs = borrowWriteIovecs(m_client->m_conn->ringBuffer());
        if (!IoVecWindow::bindFirstNonEmpty(write_iovecs, buffer, length)) {
            buffer = nullptr;
            length = 0;
            return false;
        }
        return length > 0;
    }

    void setSslSendError(const galay::ssl::SslError& error) {
        HTTP_LOG_ERROR("[h2] [send-fail] [{}]", error.message());
        m_error = Http2ErrorCode::InternalError;
    }

    void setSslRecvError(const galay::ssl::SslError& error) {
        HTTP_LOG_ERROR("[h2] [recv-fail] [{}]", error.message());
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            m_error = Http2ErrorCode::ConnectError;
            return;
        }
        m_error = Http2ErrorCode::InternalError;
    }

    bool parseNextFrame(Http2Frame::uptr& frame) {
        return parseFrameFromRingBuffer(
            m_client->m_conn->ringBuffer(),
            m_client->m_conn->peerSettings().max_frame_size,
            m_error,
            m_frame_scratch,
            frame);
    }

    bool hasPendingControlSend() const {
        return m_control_send_offset < m_control_send_buffer.size();
    }

    void scheduleSettingsAck() {
        if (hasPendingControlSend()) {
            return;
        }
        Http2SettingsFrame ack;
        ack.setAck(true);
        m_control_send_buffer = ack.serialize();
        m_control_send_offset = 0;
    }

    bool decodeHeaderBlock() {
        auto decode_result = m_client->m_conn->decoder().decode(m_header_block);
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
        if (m_error.has_value() || m_result.has_value()) {
            return true;
        }

        while (true) {
            Http2Frame::uptr frame;
            if (!parseNextFrame(frame)) {
                return m_error.has_value() || m_result.has_value();
            }

            if (!frame) {
                continue;
            }

            switch (frame->type()) {
                case Http2FrameType::Settings: {
                    auto* settings = static_cast<Http2SettingsFrame*>(frame.get());
                    if (settings->isAck()) {
                        m_client->m_conn->markSettingsAckReceived();
                    } else {
                        auto error = m_client->m_conn->peerSettings().applySettings(*settings);
                        if (error != Http2ErrorCode::NoError) {
                            m_error = error;
                            return true;
                        }
                        m_client->m_conn->encoder().setMaxTableSize(
                            m_client->m_conn->peerSettings().header_table_size);
                        scheduleSettingsAck();
                    }
                    continue;
                }
                case Http2FrameType::Headers: {
                    auto* headers = static_cast<Http2HeadersFrame*>(frame.get());
                    if (frame->streamId() != m_stream_id) {
                        continue;
                    }

                    m_header_block.append(headers->headerBlock());
                    if (headers->isEndHeaders()) {
                        if (!decodeHeaderBlock()) {
                            return true;
                        }
                        if (headers->isEndStream()) {
                            m_result = std::optional<Http2Response>(std::move(m_response_data));
                            return true;
                        }
                    }
                    continue;
                }
                case Http2FrameType::Continuation: {
                    auto* continuation = static_cast<Http2ContinuationFrame*>(frame.get());
                    if (frame->streamId() != m_stream_id) {
                        continue;
                    }

                    m_header_block.append(continuation->headerBlock());
                    if (continuation->isEndHeaders()) {
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
                        m_result = std::optional<Http2Response>(std::move(m_response_data));
                        return true;
                    }
                    continue;
                }
                case Http2FrameType::GoAway:
                    if (auto* goaway = frame->asGoAway()) {
                        m_client->m_conn->markGoawayReceived(
                            goaway->lastStreamId(),
                            goaway->errorCode(),
                            goaway->debugData());
                        m_error = (m_stream_id > goaway->lastStreamId())
                            ? Http2ErrorCode::RefusedStream
                            : Http2ErrorCode::ProtocolError;
                    } else {
                        m_error = Http2ErrorCode::ProtocolError;
                    }
                    return true;
                case Http2FrameType::RstStream:
                    if (frame->streamId() == m_stream_id) {
                        m_error = Http2ErrorCode::StreamClosed;
                        return true;
                    }
                    continue;
                default:
                    continue;
            }
        }
    }

    H2Client* m_client = nullptr;
    Http2Request m_request;
    std::string m_send_buffer;
    size_t m_send_offset = 0;
    std::string m_control_send_buffer;
    size_t m_control_send_offset = 0;
    std::string m_header_block;
    Http2Response m_response_data;
    uint32_t m_stream_id = 0;
    std::vector<uint8_t> m_frame_scratch;
    std::optional<Http2ErrorCode> m_error;
    std::optional<result_type> m_result;
    bool m_initialized = false;
};

inline auto buildRequestOperation(H2Client& client, Http2Request&& request) {
    using ResultType = H2RequestMachine::result_type;
    auto machine = H2RequestMachine(&client, std::move(request));
    return galay::ssl::SslAwaitableBuilder<ResultType>::fromStateMachine(
               client.m_conn ? client.m_conn->socket().controller() : client.m_dummy_socket.controller(),
               client.m_conn ? &client.m_conn->socket() : &client.m_dummy_socket,
               std::move(machine))
        .build();
}

class H2ClientCloseAwaitable
    : public TimeoutSupport<H2ClientCloseAwaitable>
{
public:
    using ManagerShutdownStep = Http2StreamManagerImpl<galay::ssl::SslSocket>::ShutdownAwaitable;
    using SocketCloseStep = galay::kernel::CloseAwaitable;

    explicit H2ClientCloseAwaitable(H2Client& client)
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
            if (auto* manager = m_client.m_conn->streamManager()) {
                m_manager_shutdown_step.emplace(manager, Http2ErrorCode::NoError);
                return m_manager_shutdown_step->await_suspend(handle);
            }

            m_socket_close_step.emplace(m_client.m_conn->close());
            return m_socket_close_step->await_suspend(handle);
        }

        if (m_client.m_socket) {
            m_socket_close_step.emplace(m_client.m_socket->close());
            return m_socket_close_step->await_suspend(handle);
        }

        return false;
    }

    std::expected<bool, Http2ErrorCode> await_resume() {
        if (m_manager_shutdown_step.has_value()) {
            m_manager_shutdown_step->await_resume();
        }

        if (m_socket_close_step.has_value()) {
            (void)m_socket_close_step->await_resume();
        }

        m_client.m_conn.reset();
        m_client.m_socket.reset();
        return m_client.m_close_result;
    }

private:
    H2Client& m_client;
    bool m_started = false;
    std::optional<ManagerShutdownStep> m_manager_shutdown_step;
    std::optional<SocketCloseStep> m_socket_close_step;
};

} // namespace detail

inline H2Client::H2Client(const H2ClientConfig& config)
    : m_config(config)
    , m_connected(false)
    , m_next_stream_id(1)
    , m_ssl_ctx(galay::ssl::SslMethod::TLS_Client)
{
    m_ssl_ctx.setALPNProtocols({"h2"});

    if (config.verify_peer) {
        m_ssl_ctx.setVerifyMode(galay::ssl::SslVerifyMode::Peer);
        if (!config.ca_path.empty()) {
            m_ssl_ctx.loadCACertificate(config.ca_path);
        } else {
            m_ssl_ctx.useDefaultCA();
        }
    } else {
        m_ssl_ctx.setVerifyMode(galay::ssl::SslVerifyMode::None);
    }
}

inline auto H2Client::connect(const std::string& host, uint16_t port) {
    return detail::H2ClientConnectSequence(*this, host, port);
}

inline auto H2Client::request(Http2Request request) {
    return detail::buildRequestOperation(*this, std::move(request));
}

inline bool H2Client::ensureActiveStreamManager() {
    if (!m_connected || !m_conn) {
        return false;
    }

    if (m_conn->streamManager()) {
        return true;
    }

    if (m_scheduler == nullptr) {
        HTTP_LOG_ERROR("[h2] [stream-manager] [missing-scheduler]");
        return false;
    }

    m_conn->localSettings().from(m_config);
    m_conn->runtimeConfig().from(m_config);
    m_conn->initStreamManager();

    auto* manager = m_conn->streamManager();
    if (manager == nullptr) {
        HTTP_LOG_ERROR("[h2] [stream-manager] [init-fail]");
        return false;
    }

    manager->startWithScheduler(
        m_scheduler,
        [](Http2Stream::ptr) -> galay::kernel::Coroutine { co_return; });
    return true;
}

inline Http2Stream::ptr H2Client::get(const std::string& path) {
    if (!ensureActiveStreamManager()) {
        HTTP_LOG_ERROR("[h2] [get] [not-ready]");
        return nullptr;
    }

    auto* manager = m_conn->streamManager();
    auto stream = manager->allocateStream();
    std::vector<Http2HeaderField> headers;
    headers.reserve(4);
    headers.emplace_back(":method", "GET");
    headers.emplace_back(":scheme", "https");
    headers.emplace_back(":authority", m_authority);
    headers.emplace_back(":path", path.empty() ? "/" : path);
    stream->sendHeaders(headers, true);
    return stream;
}

inline Http2Stream::ptr H2Client::post(const std::string& path,
                                       const std::string& body,
                                       const std::string& content_type) {
    if (!ensureActiveStreamManager()) {
        HTTP_LOG_ERROR("[h2] [post] [not-ready]");
        return nullptr;
    }

    auto* manager = m_conn->streamManager();
    auto stream = manager->allocateStream();
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

inline auto H2Client::close() {
    return detail::H2ClientCloseAwaitable(*this);
}

inline H2Client H2ClientBuilder::build() const { return H2Client(m_config); }

#endif // GALAY_HTTP_SSL_ENABLED

} // namespace galay::http2

#endif // GALAY_H2_CLIENT_H
