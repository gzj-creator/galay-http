#ifndef GALAY_H2C_CLIENT_H
#define GALAY_H2C_CLIENT_H

#include "Http2Conn.h"
#include "Http2Stream.h"
#include "Http2StreamManager.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/kernel/http/HttpLog.h"
#include "galay-http/utils/Http1_1RequestBuilder.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/common/Buffer.h"
#include "galay-kernel/kernel/Awaitable.h"
#include "galay-kernel/kernel/Coroutine.h"
#include "galay-kernel/kernel/Timeout.hpp"
#include <galay-utils/algorithm/Base64.hpp>
#include <memory>
#include <algorithm>
#include <string>
#include <cstring>
#include <optional>

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
class H2cShutdownAwaitable;

/**
 * @brief H2c 升级 CustomAwaitable
 * @details IO 任务链：SEND upgrade → RECV 101 → SEND preface+settings → RECV settings → SEND ACK
 */
class H2cUpgradeAwaitable
    : public CustomAwaitable
    , public TimeoutSupport<H2cUpgradeAwaitable>
{
public:
    class ProtocolSendAwaitable : public SendAwaitable
    {
    public:
        ProtocolSendAwaitable(H2cUpgradeAwaitable* owner,
                              const std::string* payload,
                              bool finish_upgrade)
            : SendAwaitable(owner->m_socket->controller(),
                            payload ? payload->data() : nullptr,
                            payload ? payload->size() : 0)
            , m_owner(owner), m_payload(payload), m_finish_upgrade(finish_upgrade)
            , m_offset(0), m_completed(!payload || payload->empty())
        {}

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle) override {
            if (m_owner->m_error.has_value()) return true;
            if (m_completed) {
                if (m_finish_upgrade) return m_owner->finishUpgrade();
                return true;
            }
            if (cqe == nullptr) { syncSendWindow(); return m_length == 0; }

            auto result = galay::kernel::io::handleSend(cqe);
            if (!result && IOError::contains(result.error().code(), kNotReady)) return false;
            if (!result) { m_owner->setSendError(result.error()); return true; }

            size_t sent = result.value();
            if (sent == 0) return false;
            m_offset += sent;
            if (m_offset >= m_payload->size()) {
                m_completed = true;
                if (m_finish_upgrade) return m_owner->finishUpgrade();
                return true;
            }
            syncSendWindow();
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
                auto result = galay::kernel::io::handleSend(handle, m_buffer, m_length);
                if (!result && IOError::contains(result.error().code(), kNotReady)) return false;
                if (!result) { m_owner->setSendError(result.error()); return true; }

                size_t sent = result.value();
                if (sent == 0) return false;
                m_offset += sent;
                if (m_offset >= m_payload->size()) {
                    m_completed = true;
                    if (m_finish_upgrade) return m_owner->finishUpgrade();
                    return true;
                }
            }
        }
#endif

    private:
        void syncSendWindow() {
            m_buffer = m_payload->data() + m_offset;
            m_length = m_payload->size() - m_offset;
        }
        H2cUpgradeAwaitable* m_owner;
        const std::string* m_payload;
        bool m_finish_upgrade;
        size_t m_offset;
        bool m_completed;
    };

    class UpgradeRecvAwaitable : public RecvAwaitable
    {
    public:
        explicit UpgradeRecvAwaitable(H2cUpgradeAwaitable* owner)
            : RecvAwaitable(owner->m_socket->controller(), nullptr, 0)
            , m_owner(owner) {}

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle) override {
            if (m_owner->m_error.has_value()) return true;
            if (m_owner->parseUpgradeResponse()) return true;
            if (cqe == nullptr) {
                if (!prepareRecvWindow()) { m_owner->setProtocolError("RingBuffer full while waiting 101"); return true; }
                return false;
            }
            auto result = galay::kernel::io::handleRecv(cqe, m_buffer);
            if (!result && IOError::contains(result.error().code(), kNotReady)) return false;
            if (!result) { m_owner->setRecvError(result.error()); return true; }
            size_t n = result.value().size();
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
                auto result = galay::kernel::io::handleRecv(handle, m_buffer, m_length);
                if (!result && IOError::contains(result.error().code(), kNotReady)) return false;
                if (!result) { m_owner->setRecvError(result.error()); return true; }
                size_t n = result.value().size();
                if (n == 0) { m_owner->setProtocolError("peer closed while waiting 101"); return true; }
                m_owner->m_ring_buffer->produce(n);
                if (m_owner->parseUpgradeResponse()) return true;
            }
        }
#endif
    private:
        bool prepareRecvWindow() {
            auto wv = m_owner->m_ring_buffer->getWriteIovecs();
            if (wv.empty()) return false;
            m_buffer = static_cast<char*>(wv[0].iov_base);
            m_length = wv[0].iov_len;
            return m_length > 0;
        }
        H2cUpgradeAwaitable* m_owner;
    };

    class SettingsRecvAwaitable : public RecvAwaitable
    {
    public:
        explicit SettingsRecvAwaitable(H2cUpgradeAwaitable* owner)
            : RecvAwaitable(owner->m_socket->controller(), nullptr, 0)
            , m_owner(owner) {}

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle) override {
            if (m_owner->m_error.has_value()) return true;
            if (m_owner->tryConsumeSettingsFrame()) return true;
            if (cqe == nullptr) {
                if (!prepareRecvWindow()) { m_owner->setProtocolError("RingBuffer full while waiting SETTINGS"); return true; }
                return false;
            }
            auto result = galay::kernel::io::handleRecv(cqe, m_buffer);
            if (!result && IOError::contains(result.error().code(), kNotReady)) return false;
            if (!result) { m_owner->setRecvError(result.error()); return true; }
            size_t n = result.value().size();
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
                auto result = galay::kernel::io::handleRecv(handle, m_buffer, m_length);
                if (!result && IOError::contains(result.error().code(), kNotReady)) return false;
                if (!result) { m_owner->setRecvError(result.error()); return true; }
                size_t n = result.value().size();
                if (n == 0) { m_owner->setProtocolError("peer closed while waiting SETTINGS"); return true; }
                m_owner->m_ring_buffer->produce(n);
                if (m_owner->tryConsumeSettingsFrame()) return true;
            }
        }
#endif
    private:
        bool prepareRecvWindow() {
            auto wv = m_owner->m_ring_buffer->getWriteIovecs();
            if (wv.empty()) return false;
            m_buffer = static_cast<char*>(wv[0].iov_base);
            m_length = wv[0].iov_len;
            return m_length > 0;
        }
        H2cUpgradeAwaitable* m_owner;
    };

    H2cUpgradeAwaitable(H2cClient& client, const std::string& path);

    bool await_ready() const noexcept { return m_error.has_value(); }
    using CustomAwaitable::await_suspend;

    std::expected<bool, Http2Error> await_resume() {
        if (!await_ready()) onCompleted();
        if (m_error.has_value()) return std::unexpected(std::move(*m_error));
        return true;
    }

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
        auto rv = m_ring_buffer->getReadIovecs();
        if (rv.empty()) return false;
        auto [ec, consumed] = m_upgrade_response.fromIOVec(rv);
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
        auto rv = m_ring_buffer->getReadIovecs();
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

public:
    std::expected<bool, IOError> m_result;
};

/**
 * @brief H2c 关闭包装 Awaitable
 * @details 将关闭逻辑委托给 shutdownImpl() 协程，确保在同一调度器顺序执行，
 *          避免提前析构连接对象导致 reader/writer 协程 UAF。
 */
class H2cShutdownAwaitable
    : public TimeoutSupport<H2cShutdownAwaitable>
{
public:
    explicit H2cShutdownAwaitable(H2cClient& client);

    bool await_ready() const noexcept { return false; }

    template<typename Handle>
    bool await_suspend(Handle handle);

    H2cShutdownAwaitable& wait() & { return *this; }
    H2cShutdownAwaitable&& wait() && { return std::move(*this); }

    std::expected<bool, Http2Error> await_resume();

private:
    H2cClient& m_client;
    bool m_started = false;
    std::optional<galay::kernel::WaitResult> m_wait_result;
};

/**
 * @brief H2c 客户端 (HTTP/2 over cleartext)
 */
class H2cClient
{
public:
    /**
     * @brief upgrade() 的包装 Awaitable
     * @details 先 co_await H2cUpgradeAwaitable（真正的 CustomAwaitable IO），
     *          成功后启动 StreamManager（需要协程上下文）
     */
    class UpgradeAwaitable : public TimeoutSupport<UpgradeAwaitable>
    {
    public:
        UpgradeAwaitable(H2cClient& client, std::string path)
            : m_client(client), m_path(std::move(path)) {}

        bool await_ready() const noexcept { return false; }

        template<typename Handle>
        bool await_suspend(Handle handle) {
            if (!m_started) {
                m_started = true;
                m_wait_result.emplace(m_client.upgradeImpl(m_path).wait());
            }
            return m_wait_result->await_suspend(handle);
        }

        UpgradeAwaitable& wait() & { return *this; }
        UpgradeAwaitable&& wait() && { return std::move(*this); }

        std::expected<bool, Http2Error> await_resume() {
            return m_client.m_upgrade_result;
        }

    private:
        H2cClient& m_client;
        std::string m_path;
        bool m_started = false;
        std::optional<galay::kernel::WaitResult> m_wait_result;
    };

    H2cClient(const H2cClientConfig& config = H2cClientConfig(), size_t ring_buffer_size = 65536)
        : m_config(config), m_ring_buffer_size(ring_buffer_size), m_port(0), m_upgraded(false) {}

    ~H2cClient() = default;
    H2cClient(const H2cClient&) = delete;
    H2cClient& operator=(const H2cClient&) = delete;
    H2cClient(H2cClient&&) = delete;
    H2cClient& operator=(H2cClient&&) = delete;

    auto connect(const std::string& host, uint16_t port) {
        m_host = host;
        m_port = port;
        HTTP_LOG_INFO("[connect] [h2c] [{}:{}]", host, port);
        m_socket = std::make_unique<TcpSocket>(IPType::IPV4);
        m_ring_buffer = std::make_unique<RingBuffer>(m_ring_buffer_size);
        auto r = m_socket->option().handleNonBlock();
        if (!r) throw std::runtime_error("Failed to set non-blocking: " + r.error().message());
        Host server_host(IPType::IPV4, host, port);
        return m_socket->connect(server_host);
    }

    UpgradeAwaitable upgrade(const std::string& path = "/");
    Http2Stream::ptr get(const std::string& path);
    Http2Stream::ptr post(const std::string& path,
                          const std::string& body,
                          const std::string& content_type = "application/x-www-form-urlencoded");
    H2cShutdownAwaitable shutdown();

    bool isUpgraded() const { return m_upgraded; }
    Http2ConnImpl<TcpSocket>* getConn() { return m_conn.get(); }

private:
    friend class H2cUpgradeAwaitable;
    friend class H2cShutdownAwaitable;

    Coroutine upgradeImpl(const std::string& path);
    Coroutine shutdownImpl();

    H2cClientConfig m_config;
    std::string m_host;
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
        .host(client.m_host + ":" + std::to_string(client.m_port))
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

    addTask(IOEventType::SEND, &m_upgrade_send);
    addTask(IOEventType::RECV, &m_upgrade_recv);
    addTask(IOEventType::SEND, &m_preface_send);
    addTask(IOEventType::RECV, &m_settings_recv);
    addTask(IOEventType::SEND, &m_ack_send);

    HTTP_LOG_INFO("[h2c] [upgrade] [begin] [path={}]", path);
}

inline bool H2cUpgradeAwaitable::finishUpgrade() {
    if (m_error.has_value()) return true;

    m_client->m_conn = std::make_unique<Http2ConnImpl<TcpSocket>>(
        std::move(*m_client->m_socket), std::move(*m_client->m_ring_buffer));
    m_client->m_conn->localSettings().from(m_client->m_config);
    m_client->m_conn->runtimeConfig().from(m_client->m_config);
    m_client->m_conn->markSettingsSent();
    m_client->m_conn->setIsClient(true);
    m_client->m_conn->initStreamManager();

    HTTP_LOG_INFO("[h2c] [upgrade] [conn-ready]");
    return true;
}

// ============== upgradeImpl — 薄协程包装 ==============

inline Coroutine H2cClient::upgradeImpl(const std::string& path) {
    auto result = co_await H2cUpgradeAwaitable(*this, path);
    if (!result) {
        m_upgrade_result = std::unexpected(result.error());
        co_return;
    }

    // H2cUpgradeAwaitable::await_resume() 会 onCompleted() 清理 IOController，
    // 这里再释放旧 socket/ring buffer，避免提前释放导致 UAF。
    m_socket.reset();
    m_ring_buffer.reset();

    // 启动 StreamManager（需要协程上下文）
    co_await m_conn->streamManager()->startInBackground(
        [](Http2Stream::ptr) -> Coroutine { co_return; }
    );

    m_upgraded = true;
    m_upgrade_result = true;
    HTTP_LOG_INFO("[h2c] [upgrade] [done]");
    co_return;
}

inline H2cClient::UpgradeAwaitable H2cClient::upgrade(const std::string& path) {
    return UpgradeAwaitable(*this, path);
}

inline H2cShutdownAwaitable::H2cShutdownAwaitable(H2cClient& client)
    : m_client(client)
{
}

template<typename Handle>
inline bool H2cShutdownAwaitable::await_suspend(Handle handle) {
    if (!m_started) {
        m_started = true;
        m_wait_result.emplace(m_client.shutdownImpl().wait());
    }
    return m_wait_result->await_suspend(handle);
}

inline std::expected<bool, Http2Error> H2cShutdownAwaitable::await_resume() {
    return m_client.m_shutdown_result;
}

// ============== shutdownImpl — 薄协程包装 ==============

inline Coroutine H2cClient::shutdownImpl() {
    HTTP_LOG_INFO("[h2c] [shutdown] [begin] [has-conn={}] [upgraded={}]",
                  m_conn != nullptr, m_upgraded);

    if (m_conn && m_conn->streamManager()) {
        co_await m_conn->streamManager()->shutdown(Http2ErrorCode::NoError).wait();
    }

    m_conn.reset();
    m_socket.reset();
    m_ring_buffer.reset();
    m_upgraded = false;
    m_shutdown_result = true;
    HTTP_LOG_INFO("[h2c] [shutdown] [done]");
    co_return;
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
    headers.push_back({":method", "GET"});
    headers.push_back({":scheme", "http"});
    headers.push_back({":authority", m_host + ":" + std::to_string(m_port)});
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
    headers.push_back({":method", "POST"});
    headers.push_back({":scheme", "http"});
    headers.push_back({":authority", m_host + ":" + std::to_string(m_port)});
    headers.push_back({":path", path.empty() ? "/" : path});
    headers.push_back({"content-type", content_type});
    stream->sendHeaders(headers, false);
    stream->sendData(body, true);
    return stream;
}

inline H2cShutdownAwaitable H2cClient::shutdown() {
    return H2cShutdownAwaitable(*this);
}

} // namespace galay::http2

#endif // GALAY_H2C_CLIENT_H
