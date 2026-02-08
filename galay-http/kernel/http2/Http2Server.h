#ifndef GALAY_HTTP2_SERVER_H
#define GALAY_HTTP2_SERVER_H

#include "Http2Conn.h"
#include "Http2StreamManager.h"
#include "Http2Stream.h"
#include "galay-http/protoc/http2/Http2Base.h"
#include "galay-http/protoc/http2/Http2Frame.h"
#include "galay-http/kernel/http/HttpLog.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/kernel/Runtime.h"
#include "galay-kernel/kernel/Coroutine.h"
#include <memory>
#include <atomic>
#include <functional>

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
     */
    Coroutine detectProtocol(Http2ConnImpl<TcpSocket>& conn, bool& success) {
        success = false;

        // 读取足够的数据来判断协议
        std::string preface_data;
        preface_data.reserve(128);

        while (preface_data.size() < kHttp2ConnectionPrefaceLength) {
            char temp_buf[128];
            auto result = co_await conn.socket().recv(temp_buf, sizeof(temp_buf));
            if (!result) {
                co_return;
            }
            auto& bytes = result.value();
            if (bytes.size() == 0) {
                co_return;
            }
            preface_data.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }

        // 检查是否是 HTTP/2 Connection Preface
        if (preface_data.size() >= kHttp2ConnectionPrefaceLength &&
            std::memcmp(preface_data.data(), kHttp2ConnectionPreface.data(), kHttp2ConnectionPrefaceLength) == 0) {
            HTTP_LOG_DEBUG("[h2] [prior-knowledge]");

            // 将多余的数据（Connection Preface 之后的数据）放入 RingBuffer
            if (preface_data.size() > kHttp2ConnectionPrefaceLength) {
                conn.feedData(preface_data.data() + kHttp2ConnectionPrefaceLength,
                             preface_data.size() - kHttp2ConnectionPrefaceLength);
            }

            // 发送服务器 SETTINGS
            while (true) {
                auto settings_result = co_await conn.sendSettings();
                if (!settings_result) {
                    co_return;
                }
                if (settings_result.value()) break;
            }

            success = true;
            co_return;
        }

        // 检查是否是 HTTP/1.1 Upgrade 请求
        if (preface_data.size() >= 4 &&
            (preface_data.substr(0, 4) == "GET " ||
             preface_data.substr(0, 5) == "POST " ||
             preface_data.substr(0, 4) == "PUT " ||
             preface_data.substr(0, 4) == "HEAD")) {
            HTTP_LOG_DEBUG("[h1] [upgrade] [detect]");

            // 继续读取直到找到完整的 HTTP 头部
            while (preface_data.find("\r\n\r\n") == std::string::npos && preface_data.size() < 8192) {
                char temp_buf[1024];
                auto result = co_await conn.socket().recv(temp_buf, sizeof(temp_buf));
                if (!result || result.value().size() == 0) {
                    co_return;
                }
                auto& bytes = result.value();
                preface_data.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            }

            // 解析 HTTP 请求头
            size_t header_end = preface_data.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                HTTP_LOG_ERROR("[header] [invalid] [too-large]");
                co_return;
            }

            std::string header_str = preface_data.substr(0, header_end + 4);

            // 检查是否包含 Upgrade: h2c
            bool has_upgrade = header_str.find("Upgrade: h2c") != std::string::npos ||
                              header_str.find("upgrade: h2c") != std::string::npos;
            bool has_http2_settings = header_str.find("HTTP2-Settings:") != std::string::npos ||
                                     header_str.find("http2-settings:") != std::string::npos;

            if (has_upgrade && has_http2_settings) {
                HTTP_LOG_DEBUG("[h1] [upgrade] [h2c]");

                // 发送 101 Switching Protocols 响应
                std::string upgrade_response =
                    "HTTP/1.1 101 Switching Protocols\r\n"
                    "Connection: Upgrade\r\n"
                    "Upgrade: h2c\r\n"
                    "\r\n";

                // 发送响应
                size_t sent = 0;
                while (sent < upgrade_response.size()) {
                    auto send_result = co_await conn.socket().send(
                        upgrade_response.data() + sent,
                        upgrade_response.size() - sent
                    );
                    if (!send_result) {
                        HTTP_LOG_ERROR("[upgrade] [send-fail]");
                        co_return;
                    }
                    sent += send_result.value();
                }

                HTTP_LOG_DEBUG("[upgrade] [101-sent]");

                // 现在期望接收 HTTP/2 Connection Preface
                std::string preface_buf;
                preface_buf.reserve(kHttp2ConnectionPrefaceLength);

                // 检查是否已经有 preface 数据（在 header_end 之后）
                if (preface_data.size() > header_end + 4) {
                    preface_buf = preface_data.substr(header_end + 4);
                }

                // 继续读取直到获得完整的 Connection Preface
                while (preface_buf.size() < kHttp2ConnectionPrefaceLength) {
                    char temp_buf[128];
                    auto result = co_await conn.socket().recv(temp_buf, sizeof(temp_buf));
                    if (!result || result.value().size() == 0) {
                        HTTP_LOG_ERROR("[preface] [recv-fail]");
                        co_return;
                    }
                    auto& bytes = result.value();
                    preface_buf.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                }

                // 验证 Connection Preface
                if (std::memcmp(preface_buf.data(), kHttp2ConnectionPreface.data(), kHttp2ConnectionPrefaceLength) != 0) {
                    HTTP_LOG_ERROR("[preface] [invalid] [after-upgrade]");
                    co_return;
                }

                HTTP_LOG_DEBUG("[preface] [ok]");

                // 将多余的数据放入 RingBuffer
                if (preface_buf.size() > kHttp2ConnectionPrefaceLength) {
                    conn.feedData(preface_buf.data() + kHttp2ConnectionPrefaceLength,
                                 preface_buf.size() - kHttp2ConnectionPrefaceLength);
                }

                // 发送服务器 SETTINGS
                while (true) {
                    auto settings_result = co_await conn.sendSettings();
                    if (!settings_result) {
                        co_return;
                    }
                    if (settings_result.value()) break;
                }

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
