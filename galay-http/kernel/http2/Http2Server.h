#ifndef GALAY_HTTP2_SERVER_H
#define GALAY_HTTP2_SERVER_H

#include "Http2Conn.h"
#include "Http2StreamManager.h"
#include "Http2Stream.h"
#include "galay-http/protoc/http2/Http2Base.h"
#include "galay-http/protoc/http2/Http2Frame.h"
#include "galay-http/protoc/http/HttpHeader.h"
#include "galay-http/kernel/http/HttpLog.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/kernel/Runtime.h"
#include "galay-kernel/kernel/Coroutine.h"
#include <memory>
#include <atomic>
#include <functional>
#include <algorithm>
#include <cctype>

namespace galay::http2
{

using namespace galay::async;
using namespace galay::kernel;

/**
 * @brief h2c 服务器配置
 */
struct H2cServerConfig
{
    std::string host = "0.0.0.0";
    uint16_t port = 8080;
    int backlog = 128;
    size_t io_scheduler_count = 0;
    size_t compute_scheduler_count = 0;

    // HTTP/2 设置
    uint32_t max_concurrent_streams = 100;
    uint32_t initial_window_size = 65535;
    uint32_t max_frame_size = 16384;
    uint32_t max_header_list_size = 8192;
    bool enable_push = false;  // 默认禁用 Server Push（curl 不支持）
};

class H2cServerBuilder {
public:
    H2cServerBuilder& host(std::string v)              { m_config.host = std::move(v); return *this; }
    H2cServerBuilder& port(uint16_t v)                 { m_config.port = v; return *this; }
    H2cServerBuilder& backlog(int v)                   { m_config.backlog = v; return *this; }
    H2cServerBuilder& ioSchedulerCount(size_t v)       { m_config.io_scheduler_count = v; return *this; }
    H2cServerBuilder& computeSchedulerCount(size_t v)  { m_config.compute_scheduler_count = v; return *this; }
    H2cServerBuilder& maxConcurrentStreams(uint32_t v)  { m_config.max_concurrent_streams = v; return *this; }
    H2cServerBuilder& initialWindowSize(uint32_t v)    { m_config.initial_window_size = v; return *this; }
    H2cServerBuilder& maxFrameSize(uint32_t v)         { m_config.max_frame_size = v; return *this; }
    H2cServerBuilder& maxHeaderListSize(uint32_t v)    { m_config.max_header_list_size = v; return *this; }
    H2cServerBuilder& enablePush(bool v)               { m_config.enable_push = v; return *this; }
    H2cServerConfig build() const                      { return m_config; }
private:
    H2cServerConfig m_config;
};

/**
 * @brief HTTP/2 流处理器类型（每个新流创建后 spawn handler(stream)）
 */
using Http2ConnectionHandler = Http2StreamHandler;

/**
 * @brief h2c 服务器 (HTTP/2 over cleartext)
 */
class H2cServer
{
public:
    explicit H2cServer(const H2cServerConfig& config = H2cServerConfig())
        : m_runtime(config.io_scheduler_count, config.compute_scheduler_count)
        , m_config(config)
        , m_handler(nullptr)
        , m_running(false)
    {
    }
    
    ~H2cServer() {
        stop();
    }
    
    H2cServer(const H2cServer&) = delete;
    H2cServer& operator=(const H2cServer&) = delete;
    
    void start(Http2ConnectionHandler handler) {
        m_handler = std::move(handler);
        startInternal();
    }
    
    void stop() {
        if (!m_running.load()) {
            return;
        }

        m_running.store(false);
        HTTP_LOG_INFO("[h2c] [server] [stopping]");

        m_runtime.stop();
        HTTP_LOG_INFO("[h2c] [server] [stopped]");
    }
    
    bool isRunning() const {
        return m_running.load();
    }
    
    Runtime& getRuntime() {
        return m_runtime;
    }

private:
    bool startInternal() {
        if (m_running.load()) {
            HTTP_LOG_WARN("[server] [already-running]");
            return false;
        }

        if (!m_handler) {
            HTTP_LOG_ERROR("[handler] [missing]");
            return false;
        }

        m_runtime.start();

        m_running.store(true);
        HTTP_LOG_INFO("[server] [listen] [h2c] [{}:{}]", m_config.host, m_config.port);

        // Spawn one serverLoop per IO scheduler with SO_REUSEPORT
        size_t io_scheduler_count = m_runtime.getIOSchedulerCount();
        for (size_t i = 0; i < io_scheduler_count; i++) {
            auto* scheduler = m_runtime.getIOScheduler(i);
            if (scheduler) {
                scheduler->spawn(serverLoop(scheduler));
            }
        }

        return true;
    }

    Coroutine serverLoop(IOScheduler* scheduler) {
        // Each serverLoop creates its own listener socket
        TcpSocket listener(IPType::IPV4);

        auto reuse_result = listener.option().handleReuseAddr();
        if (!reuse_result) {
            HTTP_LOG_ERROR("[socket] [reuseaddr-fail] [{}]", reuse_result.error().message());
            co_return;
        }

        auto reuse_port_result = listener.option().handleReusePort();
        if (!reuse_port_result) {
            HTTP_LOG_ERROR("[socket] [reuseport-fail] [{}]", reuse_port_result.error().message());
            co_return;
        }

        auto nonblock_result = listener.option().handleNonBlock();
        if (!nonblock_result) {
            HTTP_LOG_ERROR("[socket] [nonblock-fail] [{}]", nonblock_result.error().message());
            co_return;
        }

        Host bind_host(IPType::IPV4, m_config.host, m_config.port);
        auto bind_result = listener.bind(bind_host);
        if (!bind_result) {
            HTTP_LOG_ERROR("[bind] [fail] [{}:{}] [{}]", m_config.host, m_config.port, bind_result.error().message());
            co_return;
        }

        auto listen_result = listener.listen(m_config.backlog);
        if (!listen_result) {
            HTTP_LOG_ERROR("[listen] [fail] [{}]", listen_result.error().message());
            co_return;
        }

        while (m_running.load()) {
            Host client_host;
            auto accept_result = co_await listener.accept(&client_host);

            if (!accept_result) {
                if (m_running.load()) {
                    HTTP_LOG_ERROR("[accept] [fail] [{}]", accept_result.error().message());
                }
                continue;
            }

            HTTP_LOG_INFO("[connect] [h2c] [{}:{}]", client_host.ip(), client_host.port());

            TcpSocket client_socket(accept_result.value());
            auto nonblock_result = client_socket.option().handleNonBlock();
            if (!nonblock_result) {
                HTTP_LOG_ERROR("[socket] [nonblock-fail] [client] [{}]", nonblock_result.error().message());
                continue;
            }

            // Handle connection on the same scheduler
            scheduler->spawn(handleConnection(std::move(client_socket)));
        }

        co_return;
    }
    
    /**
     * @brief 处理新连接
     */
    Coroutine handleConnection(TcpSocket socket) {
        Http2ConnImpl<TcpSocket> conn(std::move(socket));

        // 配置本地设置
        conn.localSettings().from(m_config);

        // 检测协议（Prior Knowledge 或 HTTP/1.1 升级）
        bool detect_success = false;
        co_await detectProtocol(conn, detect_success).wait();
        if (!detect_success) {
            HTTP_LOG_ERROR("[protocol] [detect-fail]");
            co_await conn.close();
            co_return;
        }

        // 初始化 StreamManager 并启动帧分发循环
        conn.initStreamManager();
        auto* mgr = conn.streamManager();
        HTTP_LOG_DEBUG("[h2] [stream-mgr] [starting]");
        co_await mgr->start(m_handler).wait();
        HTTP_LOG_DEBUG("[h2] [stream-mgr] [stopped]");
        co_await conn.close();
        co_return;
    }
    
    /**
     * @brief 检测协议类型并完成初始握手
     * @param conn HTTP/2 连接
     * @param success 输出参数，表示是否成功
     *
     * 数据直接读入 conn 的 RingBuffer，避免 recv → 临时 buffer → feedData 的多余拷贝。
     * Prior Knowledge 路径：readv → RingBuffer → peek 验证 → consume preface，后续帧留在 buffer。
     * Upgrade 路径：HTTP/1.1 头取出解析后，Connection Preface 同样走 RingBuffer。
     */
    Coroutine detectProtocol(Http2ConnImpl<TcpSocket>& conn, bool& success) {
        success = false;
        auto& rb = conn.ringBuffer();

        // 直接 readv 到 RingBuffer，凑够 Connection Preface 长度
        while (rb.readable() < kHttp2ConnectionPrefaceLength) {
            auto result = co_await conn.socket().readv(rb.getWriteIovecs());
            if (!result || result.value() == 0) co_return;
            rb.produce(result.value());
        }

        // 从 RingBuffer peek 出数据判断协议
        char peek_buf[kHttp2ConnectionPrefaceLength];
        {
            auto iovecs = rb.getReadIovecs();
            size_t copied = 0;
            for (const auto& iov : iovecs) {
                size_t n = std::min(iov.iov_len, kHttp2ConnectionPrefaceLength - copied);
                std::memcpy(peek_buf + copied, iov.iov_base, n);
                copied += n;
                if (copied >= kHttp2ConnectionPrefaceLength) break;
            }
        }

        // ===== Prior Knowledge =====
        if (std::memcmp(peek_buf, kHttp2ConnectionPreface.data(), kHttp2ConnectionPrefaceLength) == 0) {
            HTTP_LOG_DEBUG("[h2] [prior-knowledge]");
            rb.consume(kHttp2ConnectionPrefaceLength);

            auto settings_result = co_await conn.sendSettings();
            if (!settings_result) co_return;
            success = true;
            co_return;
        }

        // ===== HTTP/1.1 Upgrade =====
        if (std::memcmp(peek_buf, "GET ", 4) == 0 ||
            std::memcmp(peek_buf, "POST", 4) == 0 ||
            std::memcmp(peek_buf, "PUT ", 4) == 0 ||
            std::memcmp(peek_buf, "HEAD", 4) == 0) {
            HTTP_LOG_DEBUG("[h1] [upgrade] [detect]");

            // HTTP/1.1 头不是 HTTP/2 帧，取出到 string 解析
            std::string header_data;
            header_data.reserve(4096);
            {
                auto iovecs = rb.getReadIovecs();
                for (const auto& iov : iovecs) {
                    header_data.append(static_cast<const char*>(iov.iov_base), iov.iov_len);
                }
                rb.consume(rb.readable());
            }

            while (header_data.find("\r\n\r\n") == std::string::npos && header_data.size() < 8192) {
                auto result = co_await conn.socket().readv(rb.getWriteIovecs());
                if (!result || result.value() == 0) co_return;
                rb.produce(result.value());
                auto iovecs = rb.getReadIovecs();
                for (const auto& iov : iovecs) {
                    header_data.append(static_cast<const char*>(iov.iov_base), iov.iov_len);
                }
                rb.consume(rb.readable());
            }

            size_t header_end = header_data.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                HTTP_LOG_ERROR("[header] [invalid] [too-large]");
                co_return;
            }

            galay::http::HttpRequestHeader upgrade_request;
            auto parse_result = upgrade_request.fromString(
                std::string_view(header_data.data(), header_end + 4));
            if (parse_result.first != galay::http::kNoError || parse_result.second <= 0) {
                HTTP_LOG_ERROR("[upgrade] [parse-fail]");
                co_return;
            }

            auto& headers = upgrade_request.headerPairs();
            std::string upgrade_value = headers.getValue("Upgrade");
            std::transform(upgrade_value.begin(), upgrade_value.end(), upgrade_value.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });

            bool has_upgrade = (upgrade_value == "h2c");
            bool has_http2_settings = headers.hasKey("HTTP2-Settings");

            if (has_upgrade && has_http2_settings) {
                HTTP_LOG_DEBUG("[h1] [upgrade] [h2c]");

                static constexpr char kUpgradeResp[] =
                    "HTTP/1.1 101 Switching Protocols\r\n"
                    "Connection: Upgrade\r\n"
                    "Upgrade: h2c\r\n"
                    "\r\n";
                static constexpr size_t kUpgradeRespLen = sizeof(kUpgradeResp) - 1;

                size_t sent = 0;
                while (sent < kUpgradeRespLen) {
                    auto send_result = co_await conn.socket().send(
                        kUpgradeResp + sent, kUpgradeRespLen - sent);
                    if (!send_result) {
                        HTTP_LOG_ERROR("[upgrade] [send-fail]");
                        co_return;
                    }
                    sent += send_result.value();
                }
                HTTP_LOG_DEBUG("[upgrade] [101-sent]");

                // HTTP 头后面可能已带部分 Connection Preface，写入 RingBuffer
                if (header_data.size() > header_end + 4) {
                    rb.write(header_data.data() + header_end + 4,
                             header_data.size() - header_end - 4);
                }

                // readv 直接读入 RingBuffer，凑够 Connection Preface
                while (rb.readable() < kHttp2ConnectionPrefaceLength) {
                    auto result = co_await conn.socket().readv(rb.getWriteIovecs());
                    if (!result || result.value() == 0) {
                        HTTP_LOG_ERROR("[preface] [recv-fail]");
                        co_return;
                    }
                    rb.produce(result.value());
                }

                // 从 RingBuffer 原地验证 Connection Preface
                {
                    auto iovecs = rb.getReadIovecs();
                    size_t copied = 0;
                    for (const auto& iov : iovecs) {
                        size_t n = std::min(iov.iov_len, kHttp2ConnectionPrefaceLength - copied);
                        std::memcpy(peek_buf + copied, iov.iov_base, n);
                        copied += n;
                        if (copied >= kHttp2ConnectionPrefaceLength) break;
                    }
                }
                if (std::memcmp(peek_buf, kHttp2ConnectionPreface.data(), kHttp2ConnectionPrefaceLength) != 0) {
                    HTTP_LOG_ERROR("[preface] [invalid] [after-upgrade]");
                    co_return;
                }
                HTTP_LOG_DEBUG("[preface] [ok]");

                // 消费 preface，后续数据（客户端 SETTINGS 等）留在 RingBuffer
                rb.consume(kHttp2ConnectionPrefaceLength);

                auto settings_result = co_await conn.sendSettings();
                if (!settings_result) co_return;
                success = true;
                co_return;
            }
        }

        HTTP_LOG_WARN("[protocol] [unknown] [h2c]");
        co_return;
    }

private:
    Runtime m_runtime;
    H2cServerConfig m_config;
    Http2ConnectionHandler m_handler;
    std::atomic<bool> m_running;
};

} // namespace galay::http2

#endif // GALAY_HTTP2_SERVER_H
