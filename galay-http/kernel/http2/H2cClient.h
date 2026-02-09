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
#include "galay-kernel/kernel/Coroutine.h"
#include "galay-kernel/common/Sleep.hpp"
#include <galay-utils/algorithm/Base64.hpp>
#include <memory>
#include <algorithm>
#include <string>
#include <cstring>

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
        size_t offset = 0;
        while (offset < send_buffer.size()) {
            auto result = co_await m_socket->send(send_buffer.data() + offset, send_buffer.size() - offset);
            if (!result) {
                HTTP_LOG_ERROR("[h2c] [upgrade] [send-fail] [{}]", result.error().message());
                co_return;
            }
            offset += result.value();
        }
    }

    // 3. 接收 101 Switching Protocols 响应
    HTTP_LOG_INFO("[h2c] [upgrade] [wait]");
    HttpResponse upgrade_response;
    while (!upgrade_response.isComplete()) {
        auto result = co_await m_socket->readv(m_ring_buffer->getWriteIovecs());
        if (!result) {
            HTTP_LOG_ERROR("[h2c] [upgrade] [recv-fail] [{}]", result.error().message());
            co_return;
        }
        m_ring_buffer->produce(result.value());

        auto [err, consumed] = upgrade_response.fromIOVec(m_ring_buffer->getReadIovecs());
        if (consumed > 0) {
            m_ring_buffer->consume(consumed);
        }
        if (err != HttpErrorCode::kNoError && err != HttpErrorCode::kIncomplete && err != HttpErrorCode::kHeaderInComplete) {
            HTTP_LOG_ERROR("[h2c] [upgrade] [parse-fail] [code={}]", static_cast<int>(err));
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
        size_t offset = 0;
        while (offset < kHttp2ConnectionPrefaceLength) {
            auto result = co_await m_socket->send(
                kHttp2ConnectionPreface.data() + offset, kHttp2ConnectionPrefaceLength - offset);
            if (!result) {
                HTTP_LOG_ERROR("[h2c] [preface] [send-fail] [{}]", result.error().message());
                co_return;
            }
            offset += result.value();
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

        size_t offset = 0;
        while (offset < settings_data.size()) {
            auto result = co_await m_socket->send(settings_data.data() + offset, settings_data.size() - offset);
            if (!result) {
                HTTP_LOG_ERROR("[h2c] [settings] [send-fail] [{}]", result.error().message());
                co_return;
            }
            offset += result.value();
        }
    }
    HTTP_LOG_INFO("[h2c] [settings] [sent] [wait]");

    // 6. 接收服务端 SETTINGS 帧
    while (true) {
        auto read_iovecs = m_ring_buffer->getReadIovecs();
        size_t available = 0;
        for (const auto& iov : read_iovecs) {
            available += iov.iov_len;
        }

        if (available >= kHttp2FrameHeaderLength) {
            // 读取帧头
            uint8_t header_buf[kHttp2FrameHeaderLength];
            size_t off = 0;
            for (const auto& iov : read_iovecs) {
                size_t to_copy = std::min(kHttp2FrameHeaderLength - off, iov.iov_len);
                std::memcpy(header_buf + off, iov.iov_base, to_copy);
                off += to_copy;
                if (off >= kHttp2FrameHeaderLength) break;
            }

            Http2FrameHeader frame_header = Http2FrameHeader::deserialize(header_buf);

            if (available >= kHttp2FrameHeaderLength + frame_header.length) {
                if (frame_header.type != Http2FrameType::Settings) {
                    HTTP_LOG_ERROR("[h2c] [settings] [unexpected] [type={}]",
                                  http2FrameTypeToString(frame_header.type));
                    co_return;
                }

                HTTP_LOG_INFO("[h2c] [settings] [recv-ok]");
                m_ring_buffer->consume(kHttp2FrameHeaderLength + frame_header.length);
                break;
            }
        }

        // 需要更多数据
        auto result = co_await m_socket->readv(m_ring_buffer->getWriteIovecs());
        if (!result) {
            HTTP_LOG_ERROR("[h2c] [settings] [recv-fail] [{}]", result.error().message());
            co_return;
        }
        m_ring_buffer->produce(result.value());
    }

    // 7. 发送 SETTINGS ACK
    {
        Http2SettingsFrame ack;
        ack.setAck(true);
        ack.header().stream_id = 0;
        std::string ack_data = ack.serialize();

        size_t offset = 0;
        while (offset < ack_data.size()) {
            auto result = co_await m_socket->send(ack_data.data() + offset, ack_data.size() - offset);
            if (!result) {
                HTTP_LOG_ERROR("[h2c] [settings-ack] [send-fail] [{}]", result.error().message());
                co_return;
            }
            offset += result.value();
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

        // 非 awaitable 关闭：设置 closing 标志 + shutdown(fd)，
        // 触发 readerLoop 退出，避免 co_await close() 的调度问题
        m_conn->initiateClose();

        // 等待 start() 自然完成
        while (mgr->isRunning()) {
            co_await galay::kernel::sleep(std::chrono::milliseconds(1));
        }
    }

    m_upgraded = false;
    co_return;
}

} // namespace galay::http2

#endif // GALAY_H2C_CLIENT_H
