#ifndef GALAY_H2C_CLIENT_H
#define GALAY_H2C_CLIENT_H

#include "Http2Conn.h"
#include "Http2Reader.h"
#include "Http2Writer.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/kernel/http/HttpLog.h"
#include "galay-http/utils/Http1_1RequestBuilder.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/common/Buffer.h"
#include "galay-kernel/kernel/Awaitable.h"
#include "galay-kernel/kernel/Timeout.hpp"
#include <galay-utils/algorithm/Base64.hpp>
#include <memory>
#include <optional>
#include <algorithm>

namespace galay::http2
{

using namespace galay::kernel;
using namespace galay::http;
using namespace galay::async;

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

// 前向声明
class H2cClient;

/**
 * @brief H2c 升级等待体
 * @details 参考 WSClient 的实现模式，直接使用 Socket 进行升级
 */
class H2cUpgradeAwaitable : public TimeoutSupport<H2cUpgradeAwaitable>
{
public:
    using SendAwaitableType = decltype(std::declval<TcpSocket>().send(std::declval<const char*>(), std::declval<size_t>()));
    using ReadvAwaitableType = decltype(std::declval<TcpSocket>().readv(std::declval<std::vector<iovec>>()));

    H2cUpgradeAwaitable(H2cClient* client, std::string path);

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle);

    std::expected<std::optional<bool>, Http2ErrorCode> await_resume();

    bool isInvalid() const { return m_state == State::Invalid; }
    void reset();

private:
    enum class State {
        Invalid,
        SendingUpgrade,
        ReceivingUpgradeResponse,
        SendingPreface,
        SendingSettings,
        ReceivingSettings,
        SendingSettingsAck
    };

    H2cClient* m_client;
    std::string m_path;
    State m_state;

    std::optional<SendAwaitableType> m_send_awaitable;
    std::optional<ReadvAwaitableType> m_recv_awaitable;

    std::string m_send_buffer;
    size_t m_send_offset;

    HttpRequest m_upgrade_request;
    HttpResponse m_upgrade_response;
    std::string m_settings_frame;

public:
    std::optional<std::expected<void, IOError>> m_result;
};

/**
 * @brief H2c 请求等待体
 */
class H2cRequestAwaitable : public TimeoutSupport<H2cRequestAwaitable>
{
public:
    H2cRequestAwaitable(H2cClient* client, Http2Request request);

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle);

    std::expected<std::optional<Http2Response>, Http2ErrorCode> await_resume();

    bool isInvalid() const { return m_state == State::Invalid; }

private:
    enum class State {
        Invalid,
        SendingHeaders,
        SendingData,
        ReceivingResponse
    };

    H2cClient* m_client;
    Http2Request m_request;
    Http2Response m_response;
    State m_state;
    uint32_t m_stream_id;

    std::optional<Http2WriteFrameAwaitableImpl<TcpSocket>> m_write_awaitable;
    std::optional<Http2ReadFrameAwaitableImpl<TcpSocket>> m_read_awaitable;

public:
    std::optional<std::expected<void, IOError>> m_result;
};

/**
 * @brief H2c 关闭等待体
 */
class H2cCloseAwaitable
{
public:
    H2cCloseAwaitable(H2cClient* client);

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle);

    void await_resume();

private:
    H2cClient* m_client;
    std::optional<decltype(std::declval<Http2ConnImpl<TcpSocket>>().close())> m_close_awaitable;
};

/**
 * @brief H2c 客户端 (HTTP/2 over cleartext)
 * @details 通过 HTTP/1.1 Upgrade 机制升级到 HTTP/2，参考 WSClient 的实现模式
 */
class H2cClient
{
    friend class H2cUpgradeAwaitable;
    friend class H2cRequestAwaitable;
    friend class H2cCloseAwaitable;

public:
    H2cClient(const H2cClientConfig& config = H2cClientConfig(), size_t ring_buffer_size = 65536)
        : m_config(config)
        , m_ring_buffer_size(ring_buffer_size)
        , m_socket(nullptr)
        , m_ring_buffer(nullptr)
        , m_conn(nullptr)
        , m_upgrade_awaitable(nullptr)
        , m_connected(false)
        , m_upgraded(false)
        , m_next_stream_id(1)
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

        HTTP_LOG_INFO("Connecting to {}:{}", host, port);

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
     * @brief 升级到 HTTP/2
     */
    H2cUpgradeAwaitable& upgrade(const std::string& path = "/") {
        if (!m_socket) {
            throw std::runtime_error("H2cClient not connected. Call connect() first.");
        }

        if (!m_upgrade_awaitable || m_upgrade_awaitable->isInvalid()) {
            m_upgrade_awaitable = std::make_unique<H2cUpgradeAwaitable>(this, path);
        }

        return *m_upgrade_awaitable;
    }

    /**
     * @brief 发送 GET 请求并接收响应
     */
    H2cRequestAwaitable& get(const std::string& path) {
        if (!m_conn) {
            throw std::runtime_error("H2cClient not upgraded. Call upgrade() first.");
        }

        Http2Request request;
        request.method = "GET";
        request.path = path;
        request.scheme = "http";
        request.authority = m_host + ":" + std::to_string(m_port);

        if (!m_request_awaitable || m_request_awaitable->isInvalid()) {
            m_request_awaitable = std::make_unique<H2cRequestAwaitable>(this, std::move(request));
        }

        return *m_request_awaitable;
    }

    /**
     * @brief 发送 POST 请求并接收响应
     */
    H2cRequestAwaitable& post(const std::string& path, const std::string& body, const std::string& content_type = "application/x-www-form-urlencoded") {
        if (!m_conn) {
            throw std::runtime_error("H2cClient not upgraded. Call upgrade() first.");
        }

        Http2Request request;
        request.method = "POST";
        request.path = path;
        request.body = body;
        request.scheme = "http";
        request.authority = m_host + ":" + std::to_string(m_port);
        request.headers.push_back({"content-type", content_type});

        if (!m_request_awaitable || m_request_awaitable->isInvalid()) {
            m_request_awaitable = std::make_unique<H2cRequestAwaitable>(this, std::move(request));
        }

        return *m_request_awaitable;
    }

    /**
     * @brief 关闭连接
     */
    H2cCloseAwaitable close() {
        return H2cCloseAwaitable(this);
    }

    bool isConnected() const { return m_connected; }
    bool isUpgraded() const { return m_upgraded; }

    TcpSocket* getSocket() { return m_socket.get(); }
    Http2ConnImpl<TcpSocket>* getConn() { return m_conn.get(); }

private:
    H2cClientConfig m_config;
    std::string m_host;
    uint16_t m_port;
    size_t m_ring_buffer_size;

    std::unique_ptr<TcpSocket> m_socket;
    std::unique_ptr<RingBuffer> m_ring_buffer;
    std::unique_ptr<Http2ConnImpl<TcpSocket>> m_conn;
    std::unique_ptr<H2cUpgradeAwaitable> m_upgrade_awaitable;
    std::unique_ptr<H2cRequestAwaitable> m_request_awaitable;

    bool m_connected;
    bool m_upgraded;
    uint32_t m_next_stream_id;
};

// ============== H2cUpgradeAwaitable 实现 ==============

inline H2cUpgradeAwaitable::H2cUpgradeAwaitable(H2cClient* client, std::string path)
    : m_client(client)
    , m_path(std::move(path))
    , m_state(State::Invalid)
    , m_send_offset(0)
{
}

inline bool H2cUpgradeAwaitable::await_suspend(std::coroutine_handle<> handle) {
    if (m_state == State::Invalid) {
        m_state = State::SendingUpgrade;

        // 构建 HTTP/1.1 Upgrade 请求
        Http2SettingsFrame settings_frame;
        settings_frame.addSetting(Http2SettingsId::MaxConcurrentStreams, m_client->m_config.max_concurrent_streams);
        settings_frame.addSetting(Http2SettingsId::InitialWindowSize, m_client->m_config.initial_window_size);

        std::string settings_payload = settings_frame.serialize();
        // 跳过帧头（9字节），只编码payload
        std::string settings_base64 = galay::utils::Base64Util::Base64Encode(
            reinterpret_cast<const unsigned char*>(settings_payload.data() + 9),
            settings_payload.size() - 9
        );
        // 转换为 Base64URL 格式（替换 + 为 -, / 为 _, 移除 =）
        for (char& c : settings_base64) {
            if (c == '+') c = '-';
            else if (c == '/') c = '_';
        }
        // 移除填充字符
        settings_base64.erase(std::remove(settings_base64.begin(), settings_base64.end(), '='), settings_base64.end());

        m_upgrade_request = Http1_1RequestBuilder::get(m_path)
            .host(m_client->m_host + ":" + std::to_string(m_client->m_port))
            .header("Connection", "Upgrade, HTTP2-Settings")
            .header("Upgrade", "h2c")
            .header("HTTP2-Settings", settings_base64)
            .build();

        m_send_buffer = m_upgrade_request.toString();
        m_send_offset = 0;

        HTTP_LOG_INFO("Sending HTTP/2 upgrade request...");
    }

    if (m_state == State::SendingUpgrade) {
        size_t remaining = m_send_buffer.size() - m_send_offset;
        const char* send_ptr = m_send_buffer.data() + m_send_offset;
        m_send_awaitable.emplace(m_client->m_socket->send(send_ptr, remaining));
        return m_send_awaitable->await_suspend(handle);
    }

    if (m_state == State::ReceivingUpgradeResponse) {
        m_recv_awaitable.emplace(m_client->m_socket->readv(m_client->m_ring_buffer->getWriteIovecs()));
        return m_recv_awaitable->await_suspend(handle);
    }

    if (m_state == State::SendingPreface) {
        const char* preface_ptr = kHttp2ConnectionPreface.data() + m_send_offset;
        size_t remaining = kHttp2ConnectionPrefaceLength - m_send_offset;
        m_send_awaitable.emplace(m_client->m_socket->send(preface_ptr, remaining));
        return m_send_awaitable->await_suspend(handle);
    }

    if (m_state == State::SendingSettings) {
        const char* settings_ptr = m_settings_frame.data() + m_send_offset;
        size_t remaining = m_settings_frame.size() - m_send_offset;
        m_send_awaitable.emplace(m_client->m_socket->send(settings_ptr, remaining));
        return m_send_awaitable->await_suspend(handle);
    }

    if (m_state == State::ReceivingSettings) {
        m_recv_awaitable.emplace(m_client->m_socket->readv(m_client->m_ring_buffer->getWriteIovecs()));
        return m_recv_awaitable->await_suspend(handle);
    }

    if (m_state == State::SendingSettingsAck) {
        // 使用socket发送SETTINGS ACK（此时还没有创建Http2Conn）
        HTTP_LOG_DEBUG("SendingSettingsAck: await_suspend called, sending {} bytes", m_settings_frame.size() - m_send_offset);
        const char* ack_ptr = m_settings_frame.data() + m_send_offset;
        size_t remaining = m_settings_frame.size() - m_send_offset;
        m_send_awaitable.emplace(m_client->m_socket->send(ack_ptr, remaining));
        return m_send_awaitable->await_suspend(handle);
    }

    return false;
}

inline std::expected<std::optional<bool>, Http2ErrorCode> H2cUpgradeAwaitable::await_resume() {
    HTTP_LOG_DEBUG("H2cUpgradeAwaitable::await_resume() called, state={}", static_cast<int>(m_state));

    if (m_result.has_value() && !m_result->has_value()) {
        reset();
        return std::unexpected(Http2ErrorCode::InternalError);
    }

    if (m_state == State::SendingUpgrade) {
        auto send_result = m_send_awaitable->await_resume();

        if (!send_result) {
            HTTP_LOG_ERROR("Failed to send upgrade request: {}", send_result.error().message());
            reset();
            return std::unexpected(Http2ErrorCode::InternalError);
        }

        m_send_offset += send_result.value();

        if (m_send_offset < m_send_buffer.size()) {
            return std::nullopt;  // 继续发送
        }

        HTTP_LOG_INFO("Upgrade request sent, waiting for response...");
        m_state = State::ReceivingUpgradeResponse;
        m_send_awaitable.reset();
        m_send_offset = 0;
        return std::nullopt;

    } else if (m_state == State::ReceivingUpgradeResponse) {
        auto recv_result = m_recv_awaitable->await_resume();

        if (!recv_result) {
            HTTP_LOG_ERROR("Failed to receive upgrade response: {}", recv_result.error().message());
            reset();
            return std::unexpected(Http2ErrorCode::InternalError);
        }

        m_client->m_ring_buffer->produce(recv_result.value());

        auto parse_result = m_upgrade_response.fromIOVec(
            m_client->m_ring_buffer->getReadIovecs());

        if (parse_result.first != HttpErrorCode::kNoError) {
            HTTP_LOG_ERROR("Failed to parse upgrade response: error code {}",
                          static_cast<int>(parse_result.first));
            reset();
            return std::unexpected(Http2ErrorCode::ProtocolError);
        }

        if (!m_upgrade_response.isComplete()) {
            return std::nullopt;  // 继续接收
        }

        HTTP_LOG_INFO("Received complete upgrade response");

        if (m_upgrade_response.header().code() != HttpStatusCode::SwitchingProtocol_101) {
            HTTP_LOG_ERROR("HTTP/2 upgrade failed. Status: {} {}",
                          static_cast<int>(m_upgrade_response.header().code()),
                          httpStatusCodeToString(m_upgrade_response.header().code()));
            reset();
            return false;  // 升级失败
        }

        // 检查 Upgrade 头部
        if (!m_upgrade_response.header().headerPairs().hasKey("Upgrade")) {
            HTTP_LOG_ERROR("Missing Upgrade header in response");
            reset();
            return std::unexpected(Http2ErrorCode::ProtocolError);
        }

        std::string upgrade_value = m_upgrade_response.header().headerPairs().getValue("Upgrade");
        if (upgrade_value != "h2c") {
            HTTP_LOG_ERROR("Invalid Upgrade value: {}", upgrade_value);
            reset();
            return false;
        }

        HTTP_LOG_INFO("HTTP/2 upgrade successful!");

        size_t consumed = parse_result.second;
        m_client->m_ring_buffer->consume(consumed);

        HTTP_LOG_INFO("HTTP/2 upgrade successful!");

        // 开始发送 HTTP/2 连接前言
        m_state = State::SendingPreface;
        m_send_offset = 0;
        m_recv_awaitable.reset();
        return std::nullopt;

    } else if (m_state == State::SendingPreface) {
        auto send_result = m_send_awaitable->await_resume();

        if (!send_result) {
            HTTP_LOG_ERROR("Failed to send connection preface: {}", send_result.error().message());
            reset();
            return std::unexpected(Http2ErrorCode::InternalError);
        }

        m_send_offset += send_result.value();

        if (m_send_offset < kHttp2ConnectionPrefaceLength) {
            return std::nullopt;  // 继续发送
        }

        HTTP_LOG_INFO("Connection preface sent");

        // 准备发送 SETTINGS 帧
        Http2SettingsFrame settings;
        settings.addSetting(Http2SettingsId::MaxConcurrentStreams, m_client->m_config.max_concurrent_streams);
        settings.addSetting(Http2SettingsId::InitialWindowSize, m_client->m_config.initial_window_size);
        settings.header().stream_id = 0;
        m_settings_frame = settings.serialize();

        m_state = State::SendingSettings;
        m_send_offset = 0;
        m_send_awaitable.reset();
        return std::nullopt;

    } else if (m_state == State::SendingSettings) {
        auto send_result = m_send_awaitable->await_resume();

        if (!send_result) {
            HTTP_LOG_ERROR("Failed to send SETTINGS frame: {}", send_result.error().message());
            reset();
            return std::unexpected(Http2ErrorCode::InternalError);
        }

        m_send_offset += send_result.value();

        if (m_send_offset < m_settings_frame.size()) {
            return std::nullopt;  // 继续发送
        }

        HTTP_LOG_INFO("SETTINGS frame sent, waiting for server SETTINGS...");

        m_state = State::ReceivingSettings;
        m_send_awaitable.reset();
        m_send_offset = 0;
        return std::nullopt;

    } else if (m_state == State::ReceivingSettings) {
        auto recv_result = m_recv_awaitable->await_resume();

        if (!recv_result) {
            HTTP_LOG_ERROR("Failed to receive SETTINGS: {}", recv_result.error().message());
            reset();
            return std::unexpected(Http2ErrorCode::InternalError);
        }

        m_client->m_ring_buffer->produce(recv_result.value());

        // 尝试解析 SETTINGS 帧
        auto read_iovecs = m_client->m_ring_buffer->getReadIovecs();
        size_t available = 0;
        for (const auto& iov : read_iovecs) {
            available += iov.iov_len;
        }

        if (available < kHttp2FrameHeaderLength) {
            return std::nullopt;  // 继续接收
        }

        // 读取帧头
        uint8_t header_buf[kHttp2FrameHeaderLength];
        size_t offset = 0;
        for (const auto& iov : read_iovecs) {
            size_t to_copy = std::min(kHttp2FrameHeaderLength - offset, iov.iov_len);
            std::memcpy(header_buf + offset, iov.iov_base, to_copy);
            offset += to_copy;
            if (offset >= kHttp2FrameHeaderLength) break;
        }

        Http2FrameHeader frame_header = Http2FrameHeader::deserialize(header_buf);

        if (available < kHttp2FrameHeaderLength + frame_header.length) {
            return std::nullopt;  // 继续接收
        }

        if (frame_header.type != Http2FrameType::Settings) {
            HTTP_LOG_ERROR("Expected SETTINGS frame, got {}",
                          http2FrameTypeToString(frame_header.type));
            reset();
            return std::unexpected(Http2ErrorCode::ProtocolError);
        }

        HTTP_LOG_INFO("Received SETTINGS frame from server");

        // 消费 SETTINGS 帧
        m_client->m_ring_buffer->consume(kHttp2FrameHeaderLength + frame_header.length);

        // 发送 SETTINGS ACK
        m_state = State::SendingSettingsAck;
        m_recv_awaitable.reset();

        Http2SettingsFrame ack_frame;
        ack_frame.setAck(true);
        ack_frame.header().stream_id = 0;
        m_settings_frame = ack_frame.serialize();
        m_send_offset = 0;

        HTTP_LOG_INFO("Prepared SETTINGS ACK, will send {} bytes", m_settings_frame.size());
        return std::nullopt;

    } else if (m_state == State::SendingSettingsAck) {
        HTTP_LOG_DEBUG("SendingSettingsAck: await_resume called");
        auto send_result = m_send_awaitable->await_resume();

        if (!send_result) {
            HTTP_LOG_ERROR("Failed to send SETTINGS ACK: {}", send_result.error().message());
            reset();
            return std::unexpected(Http2ErrorCode::InternalError);
        }

        m_send_offset += send_result.value();
        HTTP_LOG_DEBUG("SendingSettingsAck: sent {} bytes, total offset {}/{}",
                      send_result.value(), m_send_offset, m_settings_frame.size());

        if (m_send_offset < m_settings_frame.size()) {
            return std::nullopt;  // 继续发送
        }

        HTTP_LOG_INFO("SETTINGS ACK sent, creating Http2Conn");

        // 现在所有握手都完成了，创建 HTTP/2 连接对象
        m_client->m_conn = std::make_unique<Http2ConnImpl<TcpSocket>>(
            std::move(*m_client->m_socket),
            std::move(*m_client->m_ring_buffer)
        );

        // 配置设置
        m_client->m_conn->localSettings().max_concurrent_streams = m_client->m_config.max_concurrent_streams;
        m_client->m_conn->localSettings().initial_window_size = m_client->m_config.initial_window_size;
        m_client->m_conn->localSettings().max_frame_size = m_client->m_config.max_frame_size;
        m_client->m_conn->localSettings().enable_push = 0;

        HTTP_LOG_INFO("Http2Conn created successfully");

        m_client->m_socket.reset();
        m_client->m_ring_buffer.reset();

        // h2c 升级使用了 stream 1，所以下一个可用的 stream ID 是 3
        m_client->m_next_stream_id = 3;

        m_client->m_upgraded = true;
        // 注意：不要在这里 reset m_upgrade_awaitable，因为我们还在它的 await_resume() 中
        // 调用者会在使用完毕后自动销毁它

        HTTP_LOG_INFO("H2cClient: upgrade completed, next stream ID will be {}", m_client->m_next_stream_id);
        return std::optional<bool>(true);  // 升级完成

    } else {
        HTTP_LOG_ERROR("await_resume called in Invalid state");
        reset();
        return std::unexpected(Http2ErrorCode::ProtocolError);
    }
}

inline void H2cUpgradeAwaitable::reset() {
    m_state = State::Invalid;
    m_send_awaitable.reset();
    m_recv_awaitable.reset();
    m_send_buffer.clear();
    m_send_offset = 0;
}

// ============== H2cRequestAwaitable 实现 ==============

inline H2cRequestAwaitable::H2cRequestAwaitable(H2cClient* client, Http2Request request)
    : m_client(client)
    , m_request(std::move(request))
    , m_state(State::Invalid)
    , m_stream_id(0)
{
}

inline bool H2cRequestAwaitable::await_suspend(std::coroutine_handle<> handle) {
    switch (m_state) {
        case State::Invalid: {
            m_state = State::SendingHeaders;
            m_stream_id = m_client->m_next_stream_id;
            m_client->m_next_stream_id += 2;
            m_client->m_conn->createStream(m_stream_id);

            std::vector<Http2HeaderField> headers;
            headers.push_back({":method", m_request.method});
            headers.push_back({":scheme", m_request.scheme});
            headers.push_back({":authority", m_request.authority});
            headers.push_back({":path", m_request.path.empty() ? "/" : m_request.path});
            for (const auto& h : m_request.headers) headers.push_back(h);

            bool end_stream = m_request.body.empty();
            m_write_awaitable.emplace(m_client->m_conn->sendHeaders(m_stream_id, headers, end_stream, true));
            return m_write_awaitable->await_suspend(handle);
        }
        case State::SendingHeaders:
            return m_write_awaitable->await_suspend(handle);
        case State::SendingData:
            if (!m_write_awaitable.has_value()) {
                m_write_awaitable.emplace(m_client->m_conn->sendDataFrame(m_stream_id, m_request.body, true));
            }
            return m_write_awaitable->await_suspend(handle);
        case State::ReceivingResponse:
            if (!m_read_awaitable.has_value()) {
                m_read_awaitable.emplace(m_client->m_conn->readFrame());
            }
            return m_read_awaitable->await_suspend(handle);
        default:
            return false;
    }
}

inline std::expected<std::optional<Http2Response>, Http2ErrorCode> H2cRequestAwaitable::await_resume() {
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
            auto stream = m_client->m_conn->getStream(m_stream_id);

            switch (frame->type()) {
                case Http2FrameType::Settings: {
                    auto* settings = static_cast<Http2SettingsFrame*>(frame.get());
                    if (!settings->isAck()) {
                        m_client->m_conn->peerSettings().applySettings(*settings);
                    }
                    return std::nullopt;
                }
                case Http2FrameType::Headers: {
                    auto* hdrs = static_cast<Http2HeadersFrame*>(frame.get());
                    if (frame->streamId() != m_stream_id) return std::nullopt;

                    if (stream) {
                        stream->appendHeaderBlock(hdrs->headerBlock());
                        if (hdrs->isEndHeaders()) {
                            auto decode_result = m_client->m_conn->decoder().decode(stream->headerBlock());
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

// ============== H2cCloseAwaitable 实现 ==============

inline H2cCloseAwaitable::H2cCloseAwaitable(H2cClient* client)
    : m_client(client)
{
}

inline bool H2cCloseAwaitable::await_suspend(std::coroutine_handle<> handle) {
    m_client->m_connected = false;
    m_client->m_upgraded = false;
    if (m_client->m_conn) {
        if (!m_close_awaitable.has_value()) {
            m_close_awaitable.emplace(m_client->m_conn->close());
        }
        return m_close_awaitable->await_suspend(handle);
    }
    return false;
}

inline void H2cCloseAwaitable::await_resume() {
    if (m_close_awaitable.has_value()) {
        m_close_awaitable->await_resume();
    }
}

} // namespace galay::http2

#endif // GALAY_H2C_CLIENT_H
