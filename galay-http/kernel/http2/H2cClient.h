#ifndef GALAY_H2C_CLIENT_H
#define GALAY_H2C_CLIENT_H

#include "Http2Conn.h"
#include "Http2Stream.h"
#include "Http2StreamManager.h"
#include "galay-http/kernel/IoVecUtils.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/kernel/http/HttpLog.h"
#include "galay-http/utils/Http1_1RequestBuilder.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/common/Buffer.h"
#include "galay-kernel/kernel/Awaitable.h"
#include "galay-kernel/kernel/Task.h"
#include "galay-kernel/kernel/Timeout.hpp"
#include <galay-utils/algorithm/Base64.hpp>
#include <memory>
#include <array>
#include <algorithm>
#include <string>
#include <cstring>
#include <optional>
#include <span>

namespace galay::http2
{

using namespace galay::kernel;
using namespace galay::http;
using namespace galay::async;

struct H2cClientConfig
{
    uint32_t max_concurrent_streams = 100;
    uint32_t initial_window_size = 65535;
    uint32_t max_frame_size = 16384;
    uint32_t max_header_list_size = 8192;
    bool ping_enabled = true;
    std::chrono::milliseconds ping_interval{30000};
    std::chrono::milliseconds ping_timeout{10000};
    std::chrono::milliseconds settings_ack_timeout{10000};
    std::chrono::milliseconds graceful_shutdown_rtt{100};
    std::chrono::milliseconds graceful_shutdown_timeout{5000};
    uint32_t flow_control_target_window = kDefaultInitialWindowSize;
    Http2FlowControlStrategy flow_control_strategy;
};

class H2cClient;

class H2cClientBuilder {
public:
    H2cClientBuilder& maxConcurrentStreams(uint32_t v)  { m_config.max_concurrent_streams = v; return *this; }
    H2cClientBuilder& initialWindowSize(uint32_t v)    { m_config.initial_window_size = v; return *this; }
    H2cClientBuilder& maxFrameSize(uint32_t v)         { m_config.max_frame_size = v; return *this; }
    H2cClientBuilder& maxHeaderListSize(uint32_t v)    { m_config.max_header_list_size = v; return *this; }
    H2cClientBuilder& pingEnabled(bool v)              { m_config.ping_enabled = v; return *this; }
    H2cClientBuilder& pingInterval(std::chrono::milliseconds v) { m_config.ping_interval = v; return *this; }
    H2cClientBuilder& pingTimeout(std::chrono::milliseconds v) { m_config.ping_timeout = v; return *this; }
    H2cClientBuilder& settingsAckTimeout(std::chrono::milliseconds v) { m_config.settings_ack_timeout = v; return *this; }
    H2cClientBuilder& gracefulShutdownRtt(std::chrono::milliseconds v) { m_config.graceful_shutdown_rtt = v; return *this; }
    H2cClientBuilder& gracefulShutdownTimeout(std::chrono::milliseconds v) { m_config.graceful_shutdown_timeout = v; return *this; }
    H2cClientBuilder& flowControlTargetWindow(uint32_t v) { m_config.flow_control_target_window = v; return *this; }
    H2cClientBuilder& flowControlStrategy(Http2FlowControlStrategy v) {
        m_config.flow_control_strategy = std::move(v);
        return *this;
    }
    H2cClient build() const;
    H2cClientConfig buildConfig() const { return m_config; }
private:
    H2cClientConfig m_config;
};

// Forward declarations
class H2cUpgradeAwaitable;

/**
 * @brief H2c 升级 CustomAwaitable
 * @details IO 任务链：SEND upgrade → RECV 101 → SEND preface+settings → RECV settings → SEND ACK
 */
class H2cUpgradeAwaitable
    : public CustomAwaitable
    , public TimeoutSupport<H2cUpgradeAwaitable>
{
public:
    class ProtocolSendAwaitable : public WritevIOContext
    {
    public:
        ProtocolSendAwaitable(H2cUpgradeAwaitable* owner,
                              const std::string* payload,
                              bool finish_upgrade)
            : WritevIOContext(m_iovec_storage, 0)
            , m_owner(owner), m_payload(payload), m_finish_upgrade(finish_upgrade)
            , m_completed(true)
        {
            m_write_state.clear();
            m_write_state.reserve(1);
            if (m_payload != nullptr && !m_payload->empty()) {
                m_write_state.append({
                    .iov_base = const_cast<char*>(m_payload->data()),
                    .iov_len = m_payload->size()
                });
                m_completed = false;
            }
            syncSendWindow();
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle) override {
            if (m_owner->m_error.has_value()) return true;
            if (m_completed) {
                if (m_finish_upgrade) return m_owner->finishUpgrade();
                return true;
            }
            if (cqe == nullptr) {
                syncSendWindow();
                return pendingIovCount() == 0;
            }

            auto result = galay::kernel::io::handleWritev(cqe);
            if (!result && IOError::contains(result.error().code(), kNotReady)) return false;
            if (!result) { m_owner->setSendError(result.error()); return true; }

            const size_t sent = result.value();
            if (sent == 0) return false;
            if (!advanceAfterWrite(sent)) {
                m_owner->setSendError(IOError(kSendFailed, 0));
                return true;
            }
            if (pendingIovCount() == 0) {
                m_completed = true;
                if (m_finish_upgrade) return m_owner->finishUpgrade();
                return true;
            }
            return false;
        }
#else
        bool handleComplete(GHandle handle) override {
            while (true) {
                if (m_owner->m_error.has_value()) return true;
                if (m_completed) {
                    if (m_finish_upgrade) return m_owner->finishUpgrade();
                    return true;
                }
                syncSendWindow();
                const int iov_count = pendingIovCount();
                if (iov_count == 0) {
                    m_completed = true;
                    if (m_finish_upgrade) return m_owner->finishUpgrade();
                    return true;
                }

                auto result = galay::kernel::io::handleWritev(handle, m_write_state.data(), iov_count);
                if (!result && IOError::contains(result.error().code(), kNotReady)) return false;
                if (!result) { m_owner->setSendError(result.error()); return true; }

                const size_t sent = result.value();
                if (sent == 0) return false;
                if (!advanceAfterWrite(sent)) {
                    m_owner->setSendError(IOError(kSendFailed, 0));
                    return true;
                }
                if (pendingIovCount() == 0) {
                    m_completed = true;
                    if (m_finish_upgrade) return m_owner->finishUpgrade();
                    return true;
                }
            }
        }
#endif

    private:
        void syncSendWindow() {
            const size_t iov_count = copyBoundedIovecs(
                m_write_state.data(),
                m_write_state.count(),
                m_iovec_storage);
            const size_t compact_count = compactIovecs(m_iovec_storage, iov_count);
            m_iovecs = std::span<const struct iovec>(m_iovec_storage.data(), compact_count);
#ifdef USE_IOURING
            m_msg.msg_iov = const_cast<struct iovec*>(m_iovecs.data());
            m_msg.msg_iovlen = m_iovecs.size();
#endif
        }

        int pendingIovCount() {
            return static_cast<int>(m_write_state.count());
        }

        bool advanceAfterWrite(size_t sent_bytes) {
            const size_t advanced = m_write_state.advance(sent_bytes);
            syncSendWindow();
            return advanced == sent_bytes;
        }
        H2cUpgradeAwaitable* m_owner;
        const std::string* m_payload;
        bool m_finish_upgrade;
        bool m_completed;
        IoVecWriteState m_write_state;
        std::array<struct iovec, 2> m_iovec_storage{};
    };

    class UpgradeRecvAwaitable : public ReadvIOContext
    {
    public:
        explicit UpgradeRecvAwaitable(H2cUpgradeAwaitable* owner)
            : ReadvIOContext(m_iovec_storage, 0)
            , m_owner(owner) {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle) override {
            if (m_owner->m_error.has_value()) return true;
            if (m_owner->parseUpgradeResponse()) return true;
            if (cqe == nullptr) {
                if (!prepareRecvWindow()) { m_owner->setProtocolError("RingBuffer full while waiting 101"); return true; }
                return false;
            }
            auto result = galay::kernel::io::handleReadv(cqe);
            if (!result && IOError::contains(result.error().code(), kNotReady)) return false;
            if (!result) { m_owner->setRecvError(result.error()); return true; }
            const size_t n = result.value();
            if (n == 0) { m_owner->setProtocolError("peer closed while waiting 101"); return true; }
            m_owner->m_ring_buffer->produce(n);
            if (m_owner->parseUpgradeResponse()) return true;
            if (!prepareRecvWindow()) { m_owner->setProtocolError("RingBuffer full while waiting 101"); return true; }
            return false;
        }
#else
        bool handleComplete(GHandle handle) override {
            if (m_owner->m_error.has_value()) return true;
            if (m_owner->parseUpgradeResponse()) return true;
            while (true) {
                if (!prepareRecvWindow()) { m_owner->setProtocolError("RingBuffer full while waiting 101"); return true; }
                auto result = galay::kernel::io::handleReadv(handle,
                                                             m_iovec_storage.data(),
                                                             static_cast<int>(m_iovecs.size()));
                if (!result && IOError::contains(result.error().code(), kNotReady)) return false;
                if (!result) { m_owner->setRecvError(result.error()); return true; }
                const size_t n = result.value();
                if (n == 0) { m_owner->setProtocolError("peer closed while waiting 101"); return true; }
                m_owner->m_ring_buffer->produce(n);
                if (m_owner->parseUpgradeResponse()) return true;
            }
        }
#endif
    private:
        bool prepareRecvWindow() {
            const size_t iov_count = m_owner->m_ring_buffer->getWriteIovecs(
                m_iovec_storage.data(), m_iovec_storage.size());
            const size_t compact_count = compactIovecs(m_iovec_storage, iov_count);
            m_iovecs = std::span<const struct iovec>(m_iovec_storage.data(), compact_count);
#ifdef USE_IOURING
            m_msg.msg_iov = const_cast<struct iovec*>(m_iovecs.data());
            m_msg.msg_iovlen = m_iovecs.size();
#endif
            return compact_count > 0;
        }
        H2cUpgradeAwaitable* m_owner;
        std::array<struct iovec, 2> m_iovec_storage{};
    };

    class SettingsRecvAwaitable : public ReadvIOContext
    {
    public:
        explicit SettingsRecvAwaitable(H2cUpgradeAwaitable* owner)
            : ReadvIOContext(m_iovec_storage, 0)
            , m_owner(owner) {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle) override {
            if (m_owner->m_error.has_value()) return true;
            if (m_owner->tryConsumeSettingsFrame()) return true;
            if (cqe == nullptr) {
                if (!prepareRecvWindow()) { m_owner->setProtocolError("RingBuffer full while waiting SETTINGS"); return true; }
                return false;
            }
            auto result = galay::kernel::io::handleReadv(cqe);
            if (!result && IOError::contains(result.error().code(), kNotReady)) return false;
            if (!result) { m_owner->setRecvError(result.error()); return true; }
            const size_t n = result.value();
            if (n == 0) { m_owner->setProtocolError("peer closed while waiting SETTINGS"); return true; }
            m_owner->m_ring_buffer->produce(n);
            if (m_owner->tryConsumeSettingsFrame()) return true;
            if (!prepareRecvWindow()) { m_owner->setProtocolError("RingBuffer full while waiting SETTINGS"); return true; }
            return false;
        }
#else
        bool handleComplete(GHandle handle) override {
            if (m_owner->m_error.has_value()) return true;
            if (m_owner->tryConsumeSettingsFrame()) return true;
            while (true) {
                if (!prepareRecvWindow()) { m_owner->setProtocolError("RingBuffer full while waiting SETTINGS"); return true; }
                auto result = galay::kernel::io::handleReadv(handle,
                                                             m_iovec_storage.data(),
                                                             static_cast<int>(m_iovecs.size()));
                if (!result && IOError::contains(result.error().code(), kNotReady)) return false;
                if (!result) { m_owner->setRecvError(result.error()); return true; }
                const size_t n = result.value();
                if (n == 0) { m_owner->setProtocolError("peer closed while waiting SETTINGS"); return true; }
                m_owner->m_ring_buffer->produce(n);
                if (m_owner->tryConsumeSettingsFrame()) return true;
            }
        }
#endif
    private:
        bool prepareRecvWindow() {
            const size_t iov_count = m_owner->m_ring_buffer->getWriteIovecs(
                m_iovec_storage.data(), m_iovec_storage.size());
            const size_t compact_count = compactIovecs(m_iovec_storage, iov_count);
            m_iovecs = std::span<const struct iovec>(m_iovec_storage.data(), compact_count);
#ifdef USE_IOURING
            m_msg.msg_iov = const_cast<struct iovec*>(m_iovecs.data());
            m_msg.msg_iovlen = m_iovecs.size();
#endif
            return compact_count > 0;
        }
        H2cUpgradeAwaitable* m_owner;
        std::array<struct iovec, 2> m_iovec_storage{};
    };

    H2cUpgradeAwaitable(H2cClient& client, const std::string& path);

    bool await_ready() const noexcept { return m_error.has_value(); }

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        m_scheduler = handle.promise().taskRefView().belongScheduler();
        return CustomAwaitable::await_suspend(handle);
    }

    std::expected<bool, Http2Error> await_resume();

private:
    friend class ProtocolSendAwaitable;
    friend class UpgradeRecvAwaitable;
    friend class SettingsRecvAwaitable;

    void setSendError(const IOError& e) {
        m_error = Http2Error(Http2ErrorCode::InternalError, e.message());
    }
    void setRecvError(const IOError& e) {
        if (IOError::contains(e.code(), kDisconnectError))
            m_error = Http2Error(Http2ErrorCode::ConnectError, "connection closed");
        else
            m_error = Http2Error(Http2ErrorCode::InternalError, e.message());
    }
    void setProtocolError(std::string msg) {
        m_error = Http2Error(Http2ErrorCode::ProtocolError, std::move(msg));
    }

    bool parseUpgradeResponse() {
        auto rv = borrowReadIovecs(*m_ring_buffer);
        if (rv.empty()) return false;
        std::vector<iovec> parse_iovecs;
        if (IoVecWindow::buildWindow(rv, parse_iovecs) == 0) {
            return false;
        }
        auto [ec, consumed] = m_upgrade_response.fromIOVec(parse_iovecs);
        if (consumed > 0) m_ring_buffer->consume(consumed);
        if (ec == HttpErrorCode::kHeaderInComplete || ec == HttpErrorCode::kIncomplete) return false;
        if (ec != HttpErrorCode::kNoError) { setProtocolError("HTTP parse error during upgrade"); return true; }
        if (!m_upgrade_response.isComplete()) return false;
        if (m_upgrade_response.header().code() != HttpStatusCode::SwitchingProtocol_101) {
            setProtocolError("expected 101, got " + std::to_string(static_cast<int>(m_upgrade_response.header().code())));
            return true;
        }
        if (!m_upgrade_response.header().headerPairs().hasKey("Upgrade")) {
            setProtocolError("missing Upgrade header"); return true;
        }
        std::string uv = m_upgrade_response.header().headerPairs().getValue("Upgrade");
        if (uv != "h2c") { setProtocolError("invalid Upgrade value: " + uv); return true; }
        HTTP_LOG_INFO("[h2c] [upgrade] [101-ok]");
        return true;
    }

    bool tryConsumeSettingsFrame() {
        auto rv = borrowReadIovecs(*m_ring_buffer);
        size_t available = 0;
        for (const auto& iov : rv) available += iov.iov_len;
        if (available < kHttp2FrameHeaderLength) return false;
        uint8_t hdr[kHttp2FrameHeaderLength];
        size_t copied = 0;
        for (const auto& iov : rv) {
            size_t tc = std::min(kHttp2FrameHeaderLength - copied, iov.iov_len);
            std::memcpy(hdr + copied, iov.iov_base, tc);
            copied += tc;
            if (copied >= kHttp2FrameHeaderLength) break;
        }
        auto fh = Http2FrameHeader::deserialize(hdr);
        size_t fs = kHttp2FrameHeaderLength + static_cast<size_t>(fh.length);
        if (available < fs) return false;
        if (fh.type != Http2FrameType::Settings) {
            setProtocolError("expected SETTINGS, got " + http2FrameTypeToString(fh.type));
            return true;
        }
        m_ring_buffer->consume(fs);
        HTTP_LOG_INFO("[h2c] [upgrade] [settings-recv-ok]");
        return true;
    }

    bool finishUpgrade();

    H2cClient* m_client;
    TcpSocket* m_socket;
    RingBuffer* m_ring_buffer;
    std::string m_upgrade_request_buf;
    std::string m_preface_settings_buf;
    std::string m_ack_buf;
    HttpResponse m_upgrade_response;
    ProtocolSendAwaitable m_upgrade_send;
    UpgradeRecvAwaitable m_upgrade_recv;
    ProtocolSendAwaitable m_preface_send;
    SettingsRecvAwaitable m_settings_recv;
    ProtocolSendAwaitable m_ack_send;
    std::optional<Http2Error> m_error;
    Scheduler* m_scheduler = nullptr;

public:
    std::expected<bool, IOError> m_result;
};

/**
 * @brief H2c 客户端 (HTTP/2 over cleartext)
 */
class H2cClient
{
public:
    H2cClient(const H2cClientConfig& config = H2cClientConfig(), size_t ring_buffer_size = 65536)
        : m_config(config), m_ring_buffer_size(ring_buffer_size), m_port(0), m_upgraded(false) {}

    ~H2cClient() = default;
    H2cClient(const H2cClient&) = delete;
    H2cClient& operator=(const H2cClient&) = delete;
    H2cClient(H2cClient&&) noexcept = default;
    H2cClient& operator=(H2cClient&&) noexcept = default;

    auto connect(const std::string& host, uint16_t port) {
        m_host = host;
        m_port = port;
        m_authority = m_host + ":" + std::to_string(m_port);
        HTTP_LOG_INFO("[connect] [h2c] [{}:{}]", host, port);
        m_socket = std::make_unique<TcpSocket>(IPType::IPV4);
        m_ring_buffer = std::make_unique<RingBuffer>(m_ring_buffer_size);
        auto r = m_socket->option().handleNonBlock();
        if (!r) throw std::runtime_error("Failed to set non-blocking: " + r.error().message());
        Host server_host(IPType::IPV4, host, port);
        return m_socket->connect(server_host);
    }

    H2cUpgradeAwaitable upgrade(const std::string& path = "/");
    Http2Stream::ptr get(const std::string& path);
    Http2Stream::ptr post(const std::string& path,
                          const std::string& body,
                          const std::string& content_type = "application/x-www-form-urlencoded");
    Task<std::expected<bool, Http2Error>> shutdown();

    bool isUpgraded() const { return m_upgraded; }
    Http2ConnImpl<TcpSocket>* getConn() { return m_conn.get(); }

private:
    friend class H2cUpgradeAwaitable;

    H2cClientConfig m_config;
    std::string m_host;
    std::string m_authority;
    uint16_t m_port;
    size_t m_ring_buffer_size;
    std::unique_ptr<TcpSocket> m_socket;
    std::unique_ptr<RingBuffer> m_ring_buffer;
    std::unique_ptr<Http2ConnImpl<TcpSocket>> m_conn;
    bool m_upgraded;
    std::expected<bool, Http2Error> m_upgrade_result{true};
    std::expected<bool, Http2Error> m_shutdown_result{true};
};

inline H2cClient H2cClientBuilder::build() const { return H2cClient(m_config); }

inline std::expected<bool, Http2Error> H2cUpgradeAwaitable::await_resume() {
    // Release the upgrade sequence's ReadWrite ownership before starting the
    // HTTP/2 stream manager. Otherwise the freshly scheduled reader loop will
    // race with this awaitable for the same socket controller and fail with
    // kNotReady.
    onCompleted();
    if (m_error.has_value()) {
        m_client->m_upgraded = false;
        m_client->m_upgrade_result = std::unexpected(*m_error);
        return std::unexpected(std::move(*m_error));
    }

    auto* manager = m_client->m_conn ? m_client->m_conn->streamManager() : nullptr;
    if (manager == nullptr) {
        auto error = Http2Error(Http2ErrorCode::InternalError, "missing stream manager");
        m_client->m_upgraded = false;
        m_client->m_upgrade_result = std::unexpected(error);
        return std::unexpected(std::move(error));
    }

    manager->startWithScheduler(
        m_scheduler,
        [](Http2Stream::ptr) -> Task<void> { co_return; });

    m_client->m_socket.reset();
    m_client->m_ring_buffer.reset();
    m_client->m_upgraded = true;
    m_client->m_upgrade_result = true;
    HTTP_LOG_INFO("[h2c] [upgrade] [conn-ready]");
    HTTP_LOG_INFO("[h2c] [upgrade] [done]");
    return true;
}

// ============== H2cUpgradeAwaitable 构造 ==============

inline H2cUpgradeAwaitable::H2cUpgradeAwaitable(H2cClient& client, const std::string& path)
    : CustomAwaitable(client.m_socket->controller())
    , m_client(&client)
    , m_socket(client.m_socket.get())
    , m_ring_buffer(client.m_ring_buffer.get())
    , m_upgrade_send(this, &m_upgrade_request_buf, false)
    , m_upgrade_recv(this)
    , m_preface_send(this, &m_preface_settings_buf, false)
    , m_settings_recv(this)
    , m_ack_send(this, &m_ack_buf, true)
    , m_result(true)
{
    if (!m_socket || !m_ring_buffer) {
        m_error = Http2Error(Http2ErrorCode::ConnectError, "not connected");
        return;
    }

    // HTTP/1.1 Upgrade 请求
    Http2SettingsFrame sf;
    sf.addSetting(Http2SettingsId::MaxConcurrentStreams, client.m_config.max_concurrent_streams);
    sf.addSetting(Http2SettingsId::InitialWindowSize, client.m_config.initial_window_size);
    std::string sp = sf.serialize();
    std::string sb = galay::utils::Base64Util::Base64Encode(
        reinterpret_cast<const unsigned char*>(sp.data() + 9), sp.size() - 9);
    for (char& c : sb) { if (c == '+') c = '-'; else if (c == '/') c = '_'; }
    sb.erase(std::remove(sb.begin(), sb.end(), '='), sb.end());

    auto req = Http1_1RequestBuilder::get(path)
        .host(client.m_authority)
        .header("Connection", "Upgrade, HTTP2-Settings")
        .header("Upgrade", "h2c")
        .header("HTTP2-Settings", sb)
        .build();
    m_upgrade_request_buf = req.toString();

    // Connection Preface + SETTINGS
    std::string preface(kHttp2ConnectionPreface.begin(), kHttp2ConnectionPreface.end());
    Http2SettingsFrame settings;
    settings.addSetting(Http2SettingsId::MaxConcurrentStreams, client.m_config.max_concurrent_streams);
    settings.addSetting(Http2SettingsId::InitialWindowSize, client.m_config.initial_window_size);
    settings.header().stream_id = 0;
    m_preface_settings_buf = std::move(preface);
    m_preface_settings_buf.append(settings.serialize());

    // SETTINGS ACK
    Http2SettingsFrame ack;
    ack.setAck(true);
    ack.header().stream_id = 0;
    m_ack_buf = ack.serialize();

    // 重建 send awaitables（buffer 地址已确定）
    m_upgrade_send = ProtocolSendAwaitable(this, &m_upgrade_request_buf, false);
    m_preface_send = ProtocolSendAwaitable(this, &m_preface_settings_buf, false);
    m_ack_send = ProtocolSendAwaitable(this, &m_ack_buf, true);

    addTask(IOEventType::WRITEV, &m_upgrade_send);
    addTask(IOEventType::READV, &m_upgrade_recv);
    addTask(IOEventType::WRITEV, &m_preface_send);
    addTask(IOEventType::READV, &m_settings_recv);
    addTask(IOEventType::WRITEV, &m_ack_send);

    HTTP_LOG_INFO("[h2c] [upgrade] [begin] [path={}]", path);
}

inline bool H2cUpgradeAwaitable::finishUpgrade() {
    if (m_error.has_value()) return true;
    if (!m_scheduler) {
        m_error = Http2Error(Http2ErrorCode::InternalError, "missing scheduler");
        return true;
    }

    // TcpSocket move also moves IOController state, including sequence owners.
    // Release the upgrade awaitable's ReadWrite ownership before moving the
    // socket into Http2Conn, otherwise the new controller inherits a stale
    // owner pointer and every later readFramesBatch() aborts with kNotReady.
    onCompleted();

    m_client->m_conn = std::make_unique<Http2ConnImpl<TcpSocket>>(
        std::move(*m_client->m_socket), std::move(*m_client->m_ring_buffer));
    m_client->m_conn->localSettings().from(m_client->m_config);
    m_client->m_conn->runtimeConfig().from(m_client->m_config);
    m_client->m_conn->markSettingsSent();
    m_client->m_conn->setIsClient(true);
    m_client->m_conn->initStreamManager();
    return true;
}

inline H2cUpgradeAwaitable H2cClient::upgrade(const std::string& path) {
    return H2cUpgradeAwaitable(*this, path);
}

// ============== get() / post() ==============

inline Http2Stream::ptr H2cClient::get(const std::string& path) {
    if (!m_conn || !m_conn->streamManager()) {
        HTTP_LOG_ERROR("[h2c] [get] [not-ready]");
        return nullptr;
    }
    auto* mgr = m_conn->streamManager();
    auto stream = mgr->allocateStream();
    std::vector<Http2HeaderField> headers;
    headers.reserve(4);
    headers.push_back({":method", "GET"});
    headers.push_back({":scheme", "http"});
    headers.push_back({":authority", m_authority});
    headers.push_back({":path", path.empty() ? "/" : path});
    stream->sendHeaders(headers, true);
    return stream;
}

inline Http2Stream::ptr H2cClient::post(const std::string& path,
                                         const std::string& body,
                                         const std::string& content_type) {
    if (!m_conn || !m_conn->streamManager()) {
        HTTP_LOG_ERROR("[h2c] [post] [not-ready]");
        return nullptr;
    }
    auto* mgr = m_conn->streamManager();
    auto stream = mgr->allocateStream();
    std::vector<Http2HeaderField> headers;
    headers.reserve(5);
    headers.push_back({":method", "POST"});
    headers.push_back({":scheme", "http"});
    headers.push_back({":authority", m_authority});
    headers.push_back({":path", path.empty() ? "/" : path});
    headers.push_back({"content-type", content_type});
    stream->sendHeaders(headers, false);
    stream->sendData(body, true);
    return stream;
}

inline Task<std::expected<bool, Http2Error>> H2cClient::shutdown() {
    m_shutdown_result = true;

    HTTP_LOG_INFO("[h2c] [shutdown] [begin] [has-conn={}] [upgraded={}]",
                  m_conn != nullptr, m_upgraded);

    if (m_conn && m_conn->streamManager()) {
        co_await m_conn->streamManager()->shutdown(Http2ErrorCode::NoError);
    } else if (m_socket) {
        auto close_result = co_await m_socket->close();
        if (!close_result) {
            m_shutdown_result = std::unexpected(
                Http2Error(Http2ErrorCode::InternalError, close_result.error().message()));
        }
    }

    m_conn.reset();
    m_socket.reset();
    m_ring_buffer.reset();
    m_upgraded = false;

    if (!m_shutdown_result) {
        co_return m_shutdown_result;
    }

    m_shutdown_result = true;
    HTTP_LOG_INFO("[h2c] [shutdown] [done]");
    co_return m_shutdown_result;
}

} // namespace galay::http2

#endif // GALAY_H2C_CLIENT_H
