#ifndef GALAY_H2C_CLIENT_H
#define GALAY_H2C_CLIENT_H

#include "Http2Conn.h"
#include "Http2Stream.h"
#include "Http2StreamManager.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/protoc/http/HttpError.h"
#include "galay-http/kernel/http/HttpLog.h"
#include "galay-http/utils/Http1_1RequestBuilder.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/common/Buffer.h"
#include "galay-kernel/kernel/Awaitable.h"
#include "galay-kernel/kernel/Coroutine.h"
#include "galay-kernel/common/Sleep.hpp"
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

class H2cSendAllAwaitable
    : public CustomAwaitable
    , public TimeoutSupport<H2cSendAllAwaitable>
{
public:
    class ProtocolSendAwaitable : public SendAwaitable
    {
    public:
        explicit ProtocolSendAwaitable(H2cSendAllAwaitable* owner)
            : SendAwaitable(owner->m_socket->controller(), owner->sendPtr(), owner->remainingBytes())
            , m_owner(owner)
        {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle) override {
            if (m_owner->remainingBytes() == 0) {
                return true;
            }

            if (cqe == nullptr) {
                syncSendWindow();
                return m_length == 0;
            }

            auto result = galay::kernel::io::handleSend(cqe);
            if (!result && galay::kernel::IOError::contains(result.error().code(), galay::kernel::kNotReady)) {
                return false;
            }
            if (!result) {
                m_owner->setSendError(result.error());
                return true;
            }

            size_t sent = result.value();
            if (sent == 0) {
                return false;
            }

            m_owner->consume(sent);
            if (m_owner->remainingBytes() == 0) {
                return true;
            }

            syncSendWindow();
            return false;
        }
#else
        bool handleComplete(GHandle handle) override {
            while (m_owner->remainingBytes() > 0) {
                syncSendWindow();

                auto result = galay::kernel::io::handleSend(handle, m_buffer, m_length);
                if (!result && galay::kernel::IOError::contains(result.error().code(), galay::kernel::kNotReady)) {
                    return false;
                }
                if (!result) {
                    m_owner->setSendError(result.error());
                    return true;
                }

                size_t sent = result.value();
                if (sent == 0) {
                    return false;
                }
                m_owner->consume(sent);
            }
            return true;
        }
#endif

    private:
        void syncSendWindow() {
            m_buffer = m_owner->sendPtr();
            m_length = m_owner->remainingBytes();
        }

        H2cSendAllAwaitable* m_owner;
    };

    H2cSendAllAwaitable(TcpSocket& socket, const char* buffer, size_t length)
        : CustomAwaitable(socket.controller())
        , m_socket(&socket)
        , m_buffer(buffer == nullptr ? "" : buffer)
        , m_total_length(length)
        , m_sent(0)
        , m_send_awaitable(this)
        , m_result(true)
    {
        if (m_total_length > 0) {
            addTask(IOEventType::SEND, &m_send_awaitable);
        }
    }

    bool await_ready() const noexcept {
        return m_total_length == 0;
    }

    using CustomAwaitable::await_suspend;

    std::expected<bool, HttpError> await_resume() {
        if (!await_ready()) {
            onCompleted();
        }

        if (!m_result.has_value()) {
            return std::unexpected(HttpError(kSendError, m_result.error().message()));
        }
        if (m_http_error.has_value()) {
            return std::unexpected(std::move(*m_http_error));
        }
        return true;
    }

private:
    friend class ProtocolSendAwaitable;

    size_t remainingBytes() const {
        return m_total_length - m_sent;
    }

    const char* sendPtr() const {
        return m_buffer + m_sent;
    }

    void consume(size_t bytes) {
        m_sent = std::min(m_total_length, m_sent + bytes);
    }

    void setSendError(const IOError& io_error) {
        m_http_error = HttpError(kSendError, io_error.message());
    }

    TcpSocket* m_socket;
    const char* m_buffer;
    size_t m_total_length;
    size_t m_sent;
    ProtocolSendAwaitable m_send_awaitable;
    std::optional<HttpError> m_http_error;

public:
    std::expected<bool, IOError> m_result;
};

class H2cReadUpgradeResponseAwaitable
    : public CustomAwaitable
    , public TimeoutSupport<H2cReadUpgradeResponseAwaitable>
{
public:
    class ProtocolRecvAwaitable : public RecvAwaitable
    {
    public:
        explicit ProtocolRecvAwaitable(H2cReadUpgradeResponseAwaitable* owner)
            : RecvAwaitable(owner->m_socket->controller(), nullptr, 0)
            , m_owner(owner)
        {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle) override {
            if (m_owner->parseResponseFromRingBuffer()) {
                return true;
            }

            if (cqe == nullptr) {
                if (!prepareRecvWindow()) {
                    m_owner->setParseError(HttpError(kHeaderTooLarge));
                    return true;
                }
                return false;
            }

            auto result = galay::kernel::io::handleRecv(cqe, m_buffer);
            if (!result && galay::kernel::IOError::contains(result.error().code(), galay::kernel::kNotReady)) {
                return false;
            }
            if (!result) {
                m_owner->setRecvError(result.error());
                return true;
            }

            size_t recv_bytes = result.value().size();
            if (recv_bytes == 0) {
                m_owner->setParseError(HttpError(kConnectionClose));
                return true;
            }

            m_owner->m_ring_buffer->produce(recv_bytes);

            if (m_owner->parseResponseFromRingBuffer()) {
                return true;
            }

            if (!prepareRecvWindow()) {
                m_owner->setParseError(HttpError(kHeaderTooLarge));
                return true;
            }
            return false;
        }
#else
        bool handleComplete(GHandle handle) override {
            if (m_owner->parseResponseFromRingBuffer()) {
                return true;
            }

            while (true) {
                if (!prepareRecvWindow()) {
                    m_owner->setParseError(HttpError(kHeaderTooLarge));
                    return true;
                }

                auto result = galay::kernel::io::handleRecv(handle, m_buffer, m_length);
                if (!result && galay::kernel::IOError::contains(result.error().code(), galay::kernel::kNotReady)) {
                    return false;
                }
                if (!result) {
                    m_owner->setRecvError(result.error());
                    return true;
                }

                size_t recv_bytes = result.value().size();
                if (recv_bytes == 0) {
                    m_owner->setParseError(HttpError(kConnectionClose));
                    return true;
                }

                m_owner->m_ring_buffer->produce(recv_bytes);

                if (m_owner->parseResponseFromRingBuffer()) {
                    return true;
                }
            }
        }
#endif

    private:
        bool prepareRecvWindow() {
            auto write_iovecs = m_owner->m_ring_buffer->getWriteIovecs();
            if (write_iovecs.empty()) {
                return false;
            }
            m_buffer = static_cast<char*>(write_iovecs[0].iov_base);
            m_length = write_iovecs[0].iov_len;
            return m_length > 0;
        }

        H2cReadUpgradeResponseAwaitable* m_owner;
    };

    H2cReadUpgradeResponseAwaitable(TcpSocket& socket, RingBuffer& ring_buffer, HttpResponse& response)
        : CustomAwaitable(socket.controller())
        , m_ring_buffer(&ring_buffer)
        , m_response(&response)
        , m_socket(&socket)
        , m_recv_awaitable(this)
        , m_result(true)
    {
        addTask(IOEventType::RECV, &m_recv_awaitable);
    }

    bool await_ready() const noexcept {
        return false;
    }

    using CustomAwaitable::await_suspend;

    std::expected<bool, HttpError> await_resume() {
        onCompleted();

        if (!m_result.has_value()) {
            const auto& io_error = m_result.error();
            if (IOError::contains(io_error.code(), kDisconnectError)) {
                return std::unexpected(HttpError(kConnectionClose));
            }
            return std::unexpected(HttpError(kRecvError, io_error.message()));
        }
        if (m_http_error.has_value()) {
            return std::unexpected(std::move(*m_http_error));
        }
        return true;
    }

private:
    bool parseResponseFromRingBuffer() {
        auto read_iovecs = m_ring_buffer->getReadIovecs();
        if (read_iovecs.empty()) {
            return false;
        }

        auto [error_code, consumed] = m_response->fromIOVec(read_iovecs);
        if (consumed > 0) {
            m_ring_buffer->consume(consumed);
        }

        if (error_code == HttpErrorCode::kHeaderInComplete || error_code == HttpErrorCode::kIncomplete) {
            return false;
        }

        if (error_code != HttpErrorCode::kNoError) {
            setParseError(HttpError(error_code));
            return true;
        }

        return m_response->isComplete();
    }

    void setRecvError(const IOError& io_error) {
        m_http_error = HttpError(kRecvError, io_error.message());
    }

    void setParseError(HttpError&& error) {
        m_http_error = std::move(error);
    }

    RingBuffer* m_ring_buffer;
    HttpResponse* m_response;
    TcpSocket* m_socket;
    ProtocolRecvAwaitable m_recv_awaitable;
    std::optional<HttpError> m_http_error;

public:
    std::expected<bool, IOError> m_result;
};

class H2cReadSettingsFrameAwaitable
    : public CustomAwaitable
    , public TimeoutSupport<H2cReadSettingsFrameAwaitable>
{
public:
    class ProtocolRecvAwaitable : public RecvAwaitable
    {
    public:
        explicit ProtocolRecvAwaitable(H2cReadSettingsFrameAwaitable* owner)
            : RecvAwaitable(owner->m_socket->controller(), nullptr, 0)
            , m_owner(owner)
        {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle) override {
            if (m_owner->tryConsumeSettingsFrame()) {
                return true;
            }

            if (cqe == nullptr) {
                if (!prepareRecvWindow()) {
                    m_owner->setProtocolError("RingBuffer is full while waiting SETTINGS");
                    return true;
                }
                return false;
            }

            auto result = galay::kernel::io::handleRecv(cqe, m_buffer);
            if (!result && galay::kernel::IOError::contains(result.error().code(), galay::kernel::kNotReady)) {
                return false;
            }
            if (!result) {
                m_owner->setRecvError(result.error());
                return true;
            }

            size_t recv_bytes = result.value().size();
            if (recv_bytes == 0) {
                m_owner->setProtocolError("peer closed while waiting SETTINGS");
                return true;
            }

            m_owner->m_ring_buffer->produce(recv_bytes);

            if (m_owner->tryConsumeSettingsFrame()) {
                return true;
            }

            if (!prepareRecvWindow()) {
                m_owner->setProtocolError("RingBuffer is full while waiting SETTINGS");
                return true;
            }
            return false;
        }
#else
        bool handleComplete(GHandle handle) override {
            if (m_owner->tryConsumeSettingsFrame()) {
                return true;
            }

            while (true) {
                if (!prepareRecvWindow()) {
                    m_owner->setProtocolError("RingBuffer is full while waiting SETTINGS");
                    return true;
                }

                auto result = galay::kernel::io::handleRecv(handle, m_buffer, m_length);
                if (!result && galay::kernel::IOError::contains(result.error().code(), galay::kernel::kNotReady)) {
                    return false;
                }
                if (!result) {
                    m_owner->setRecvError(result.error());
                    return true;
                }

                size_t recv_bytes = result.value().size();
                if (recv_bytes == 0) {
                    m_owner->setProtocolError("peer closed while waiting SETTINGS");
                    return true;
                }

                m_owner->m_ring_buffer->produce(recv_bytes);
                if (m_owner->tryConsumeSettingsFrame()) {
                    return true;
                }
            }
        }
#endif

    private:
        bool prepareRecvWindow() {
            auto write_iovecs = m_owner->m_ring_buffer->getWriteIovecs();
            if (write_iovecs.empty()) {
                return false;
            }
            m_buffer = static_cast<char*>(write_iovecs[0].iov_base);
            m_length = write_iovecs[0].iov_len;
            return m_length > 0;
        }

        H2cReadSettingsFrameAwaitable* m_owner;
    };

    H2cReadSettingsFrameAwaitable(TcpSocket& socket, RingBuffer& ring_buffer)
        : CustomAwaitable(socket.controller())
        , m_ring_buffer(&ring_buffer)
        , m_socket(&socket)
        , m_recv_awaitable(this)
        , m_result(true)
    {
        addTask(IOEventType::RECV, &m_recv_awaitable);
    }

    bool await_ready() const noexcept {
        return false;
    }

    using CustomAwaitable::await_suspend;

    std::expected<bool, Http2Error> await_resume() {
        onCompleted();

        if (!m_result.has_value()) {
            return std::unexpected(Http2Error(Http2ErrorCode::InternalError, m_result.error().message()));
        }
        if (m_http2_error.has_value()) {
            return std::unexpected(std::move(*m_http2_error));
        }
        return true;
    }

private:
    bool tryConsumeSettingsFrame() {
        auto read_iovecs = m_ring_buffer->getReadIovecs();
        size_t available = 0;
        for (const auto& iov : read_iovecs) {
            available += iov.iov_len;
        }

        if (available < kHttp2FrameHeaderLength) {
            return false;
        }

        uint8_t header_buf[kHttp2FrameHeaderLength];
        size_t copied = 0;
        for (const auto& iov : read_iovecs) {
            const size_t to_copy = std::min(kHttp2FrameHeaderLength - copied, iov.iov_len);
            std::memcpy(header_buf + copied, iov.iov_base, to_copy);
            copied += to_copy;
            if (copied >= kHttp2FrameHeaderLength) {
                break;
            }
        }

        const Http2FrameHeader frame_header = Http2FrameHeader::deserialize(header_buf);
        const size_t frame_size = kHttp2FrameHeaderLength + static_cast<size_t>(frame_header.length);
        if (available < frame_size) {
            return false;
        }

        if (frame_header.type != Http2FrameType::Settings) {
            setProtocolError("Unexpected frame type while waiting SETTINGS: " + http2FrameTypeToString(frame_header.type));
            return true;
        }

        m_ring_buffer->consume(frame_size);
        return true;
    }

    void setRecvError(const IOError& io_error) {
        m_http2_error = Http2Error(Http2ErrorCode::InternalError, io_error.message());
    }

    void setProtocolError(std::string message) {
        m_http2_error = Http2Error(Http2ErrorCode::ProtocolError, std::move(message));
    }

    RingBuffer* m_ring_buffer;
    TcpSocket* m_socket;
    ProtocolRecvAwaitable m_recv_awaitable;
    std::optional<Http2Error> m_http2_error;

public:
    std::expected<bool, IOError> m_result;
};

/**
 * @brief H2c 客户端配置
 */
struct H2cClientConfig
{
    uint32_t max_concurrent_streams = 100;
    uint32_t initial_window_size = 65535;
    uint32_t max_frame_size = 16384;
    uint32_t max_header_list_size = 8192;
};

class H2cClientBuilder {
public:
    H2cClientBuilder& maxConcurrentStreams(uint32_t v)  { m_config.max_concurrent_streams = v; return *this; }
    H2cClientBuilder& initialWindowSize(uint32_t v)    { m_config.initial_window_size = v; return *this; }
    H2cClientBuilder& maxFrameSize(uint32_t v)         { m_config.max_frame_size = v; return *this; }
    H2cClientBuilder& maxHeaderListSize(uint32_t v)    { m_config.max_header_list_size = v; return *this; }
    H2cClientConfig build() const                      { return m_config; }
private:
    H2cClientConfig m_config;
};

/**
 * @brief H2c 客户端 (HTTP/2 over cleartext)
 * @details 通过 HTTP/1.1 Upgrade 机制升级到 HTTP/2，基于 StreamManager 支持多路复用
 *
 * 使用方式:
 * @code
 * H2cClient client;
 * co_await client.connect(host, port);
 * co_await client.upgrade("/").wait();
 *
 * // 串行请求
 * auto stream = client.get("/api/data");
 * co_await stream->readResponse().wait();
 * auto& resp = stream->response();
 *
 * // 多路复用
 * auto s1 = client.get("/a");
 * auto s2 = client.post("/b", body, "application/json");
 * co_await s1->readResponse().wait();
 * co_await s2->readResponse().wait();
 *
 * co_await client.shutdown().wait();
 * @endcode
 */
class H2cClient
{
public:
    H2cClient(const H2cClientConfig& config = H2cClientConfig(), size_t ring_buffer_size = 65536)
        : m_config(config)
        , m_ring_buffer_size(ring_buffer_size)
        , m_port(0)
        , m_upgraded(false)
    {
    }

    ~H2cClient() = default;

    H2cClient(const H2cClient&) = delete;
    H2cClient& operator=(const H2cClient&) = delete;
    H2cClient(H2cClient&&) = delete;
    H2cClient& operator=(H2cClient&&) = delete;

    /**
     * @brief 连接到服务器
     */
    auto connect(const std::string& host, uint16_t port) {
        m_host = host;
        m_port = port;

        HTTP_LOG_INFO("[connect] [h2c] [{}:{}]", host, port);

        m_socket = std::make_unique<TcpSocket>(IPType::IPV4);
        m_ring_buffer = std::make_unique<RingBuffer>(m_ring_buffer_size);

        auto nonblock_result = m_socket->option().handleNonBlock();
        if (!nonblock_result) {
            throw std::runtime_error("Failed to set non-blocking: " + nonblock_result.error().message());
        }

        Host server_host(IPType::IPV4, host, port);
        return m_socket->connect(server_host);
    }

    /**
     * @brief 升级到 HTTP/2 并启动 StreamManager
     * @param path 升级请求的路径
     * @return Coroutine，使用 co_await client.upgrade("/").wait()
     */
    Coroutine upgrade(const std::string& path = "/");

    /**
     * @brief 发送 GET 请求，返回 Stream
     * @details 调用者用 co_await stream->readResponse().wait() 读取响应
     */
    Http2Stream::ptr get(const std::string& path);

    /**
     * @brief 发送 POST 请求，返回 Stream
     * @details 调用者用 co_await stream->readResponse().wait() 读取响应
     */
    Http2Stream::ptr post(const std::string& path,
                          const std::string& body,
                          const std::string& content_type = "application/x-www-form-urlencoded");

    /**
     * @brief 优雅关闭连接
     */
    Coroutine shutdown();

    bool isUpgraded() const { return m_upgraded; }

    Http2ConnImpl<TcpSocket>* getConn() { return m_conn.get(); }

private:
    H2cClientConfig m_config;
    std::string m_host;
    uint16_t m_port;
    size_t m_ring_buffer_size;

    std::unique_ptr<TcpSocket> m_socket;
    std::unique_ptr<RingBuffer> m_ring_buffer;
    std::unique_ptr<Http2ConnImpl<TcpSocket>> m_conn;
    bool m_upgraded;
};

// ============== upgrade() 实现 ==============

inline Coroutine H2cClient::upgrade(const std::string& path) {
    if (!m_socket) {
        HTTP_LOG_ERROR("[h2c] [upgrade] [not-connected]");
        co_return;
    }

    // 1. 构建 HTTP/1.1 Upgrade 请求
    Http2SettingsFrame settings_frame;
    settings_frame.addSetting(Http2SettingsId::MaxConcurrentStreams, m_config.max_concurrent_streams);
    settings_frame.addSetting(Http2SettingsId::InitialWindowSize, m_config.initial_window_size);

    std::string settings_payload = settings_frame.serialize();
    // 跳过帧头（9字节），只编码 payload
    std::string settings_base64 = galay::utils::Base64Util::Base64Encode(
        reinterpret_cast<const unsigned char*>(settings_payload.data() + 9),
        settings_payload.size() - 9
    );
    // 转换为 Base64URL 格式
    for (char& c : settings_base64) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    settings_base64.erase(std::remove(settings_base64.begin(), settings_base64.end(), '='), settings_base64.end());

    auto upgrade_request = Http1_1RequestBuilder::get(path)
        .host(m_host + ":" + std::to_string(m_port))
        .header("Connection", "Upgrade, HTTP2-Settings")
        .header("Upgrade", "h2c")
        .header("HTTP2-Settings", settings_base64)
        .build();

    std::string send_buffer = upgrade_request.toString();

    // 2. 发送 Upgrade 请求
    HTTP_LOG_INFO("[h2c] [upgrade] [send]");
    {
        auto result = co_await H2cSendAllAwaitable(*m_socket, send_buffer.data(), send_buffer.size());
        if (!result) {
            HTTP_LOG_ERROR("[h2c] [upgrade] [send-fail] [{}]", result.error().message());
            co_return;
        }
    }

    // 3. 接收 101 Switching Protocols 响应
    HTTP_LOG_INFO("[h2c] [upgrade] [wait]");
    HttpResponse upgrade_response;
    {
        auto result = co_await H2cReadUpgradeResponseAwaitable(*m_socket, *m_ring_buffer, upgrade_response);
        if (!result) {
            HTTP_LOG_ERROR("[h2c] [upgrade] [recv-fail] [{}]", result.error().message());
            co_return;
        }
    }

    HTTP_LOG_INFO("[h2c] [upgrade] [recv-ok]");

    if (upgrade_response.header().code() != HttpStatusCode::SwitchingProtocol_101) {
        HTTP_LOG_ERROR("[h2c] [upgrade] [fail] [{}] [{}]",
                      static_cast<int>(upgrade_response.header().code()),
                      httpStatusCodeToString(upgrade_response.header().code()));
        co_return;
    }

    if (!upgrade_response.header().headerPairs().hasKey("Upgrade")) {
        HTTP_LOG_ERROR("[h2c] [upgrade] [upgrade-missing]");
        co_return;
    }

    std::string upgrade_value = upgrade_response.header().headerPairs().getValue("Upgrade");
    if (upgrade_value != "h2c") {
        HTTP_LOG_ERROR("[h2c] [upgrade] [upgrade-invalid] [value={}]", upgrade_value);
        co_return;
    }

    HTTP_LOG_INFO("[h2c] [upgrade] [ok]");

    // 4. 发送 HTTP/2 Connection Preface
    {
        auto result = co_await H2cSendAllAwaitable(
            *m_socket, kHttp2ConnectionPreface.data(), kHttp2ConnectionPrefaceLength);
        if (!result) {
            HTTP_LOG_ERROR("[h2c] [preface] [send-fail] [{}]", result.error().message());
            co_return;
        }
    }
    HTTP_LOG_INFO("[h2c] [preface] [sent]");

    // 5. 发送 SETTINGS 帧
    {
        Http2SettingsFrame settings;
        settings.addSetting(Http2SettingsId::MaxConcurrentStreams, m_config.max_concurrent_streams);
        settings.addSetting(Http2SettingsId::InitialWindowSize, m_config.initial_window_size);
        settings.header().stream_id = 0;
        std::string settings_data = settings.serialize();

        auto result = co_await H2cSendAllAwaitable(*m_socket, settings_data.data(), settings_data.size());
        if (!result) {
            HTTP_LOG_ERROR("[h2c] [settings] [send-fail] [{}]", result.error().message());
            co_return;
        }
    }
    HTTP_LOG_INFO("[h2c] [settings] [sent] [wait]");

    // 6. 接收服务端 SETTINGS 帧
    {
        auto result = co_await H2cReadSettingsFrameAwaitable(*m_socket, *m_ring_buffer);
        if (!result) {
            HTTP_LOG_ERROR("[h2c] [settings] [recv-fail] [{}]", result.error().toString());
            co_return;
        }
        HTTP_LOG_INFO("[h2c] [settings] [recv-ok]");
    }

    // 7. 发送 SETTINGS ACK
    {
        Http2SettingsFrame ack;
        ack.setAck(true);
        ack.header().stream_id = 0;
        std::string ack_data = ack.serialize();

        auto result = co_await H2cSendAllAwaitable(*m_socket, ack_data.data(), ack_data.size());
        if (!result) {
            HTTP_LOG_ERROR("[h2c] [settings-ack] [send-fail] [{}]", result.error().message());
            co_return;
        }
    }
    HTTP_LOG_INFO("[h2c] [settings-ack] [sent]");

    // 8. 创建 Http2Conn + 启动 StreamManager
    m_conn = std::make_unique<Http2ConnImpl<TcpSocket>>(
        std::move(*m_socket), std::move(*m_ring_buffer));
    m_conn->localSettings().from(m_config);
    m_conn->setIsClient(true);
    m_conn->initStreamManager();

    HTTP_LOG_INFO("[h2c] [conn] [ready]");

    m_socket.reset();
    m_ring_buffer.reset();

    // StreamManager::start() 中 m_next_local_stream_id = isClient() ? 3 : 2
    // 所以客户端流 ID 从 3 开始（stream 1 被 h2c upgrade 占用）
    co_await m_conn->streamManager()->startInBackground(
        [](Http2Stream::ptr) -> Coroutine { co_return; }
    ).wait();

    m_upgraded = true;
    HTTP_LOG_INFO("[h2c] [upgrade] [done]");
    co_return;
}

// ============== get() / post() 实现 ==============

inline Http2Stream::ptr H2cClient::get(const std::string& path) {
    auto* mgr = m_conn->streamManager();
    auto stream = mgr->allocateStream();

    std::vector<Http2HeaderField> headers;
    headers.push_back({":method", "GET"});
    headers.push_back({":scheme", "http"});
    headers.push_back({":authority", m_host + ":" + std::to_string(m_port)});
    headers.push_back({":path", path.empty() ? "/" : path});

    stream->sendHeaders(headers, true);  // end_stream=true for GET
    return stream;
}

inline Http2Stream::ptr H2cClient::post(const std::string& path,
                                         const std::string& body,
                                         const std::string& content_type) {
    auto* mgr = m_conn->streamManager();
    auto stream = mgr->allocateStream();

    std::vector<Http2HeaderField> headers;
    headers.push_back({":method", "POST"});
    headers.push_back({":scheme", "http"});
    headers.push_back({":authority", m_host + ":" + std::to_string(m_port)});
    headers.push_back({":path", path.empty() ? "/" : path});
    headers.push_back({"content-type", content_type});

    stream->sendHeaders(headers, false);  // end_stream=false (has body)
    stream->sendData(body, true);         // end_stream=true
    return stream;
}

// ============== shutdown() 实现 ==============

inline Coroutine H2cClient::shutdown() {
    if (!m_conn || !m_conn->streamManager()) {
        if (m_socket) {
            co_await m_socket->close();
            m_socket.reset();
        }
        m_ring_buffer.reset();
        m_upgraded = false;
        co_return;
    }

    auto* mgr = m_conn->streamManager();

    if (mgr->isRunning()) {
        // 发送 GOAWAY 通知对端
        auto waiter = mgr->sendGoaway();
        if (waiter) {
            co_await waiter->wait();
        }

        // 先触发 readerLoop 退出，再等待管理器自然停机。
        m_conn->initiateClose();

        // 等待 start() 自然完成（reader/writer 全部退出）
        while (mgr->isRunning()) {
            co_await galay::kernel::sleep(std::chrono::milliseconds(1));
        }
    }

    // 确保执行 addClose 清理 kqueue 注册，并关闭 fd。
    // 仅 initiateClose() 不会执行 addClose，直接销毁 m_conn 可能留下悬空 udata。
    co_await m_conn->close();

    m_conn.reset();
    m_socket.reset();
    m_ring_buffer.reset();
    m_upgraded = false;
    co_return;
}

} // namespace galay::http2

#endif // GALAY_H2C_CLIENT_H
