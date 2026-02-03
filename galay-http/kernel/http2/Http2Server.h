#ifndef GALAY_HTTP2_SERVER_H
#define GALAY_HTTP2_SERVER_H

#include "Http2Conn.h"
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
 * @brief HTTP/2 请求处理器类型
 */
using Http2RequestHandler = std::function<Coroutine(Http2ConnImpl<TcpSocket>&, Http2Stream::ptr, Http2Request)>;

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
        , m_listener(nullptr)
        , m_running(false)
    {
    }
    
    ~H2cServer() {
        stop();
    }
    
    H2cServer(const H2cServer&) = delete;
    H2cServer& operator=(const H2cServer&) = delete;
    
    void start(Http2RequestHandler handler) {
        m_handler = std::move(handler);
        startInternal();
    }
    
    void stop() {
        if (!m_running.load()) {
            return;
        }
        
        m_running.store(false);
        HTTP_LOG_INFO("H2c server stopping...");
      if (m_listener) {
            m_listener.reset();
        }
        
        m_runtime.stop();
        HTTP_LOG_INFO("H2c server stopped");
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
            HTTP_LOG_WARN("server already running");
            return false;
        }
        
        if (!m_handler) {
            HTTP_LOG_ERROR("handler not set");
            return false;
        }
        
        m_runtime.start();
        
        auto* scheduler = m_runtime.getNextIOScheduler();
        if (!scheduler) {
            HTTP_LOG_ERROR("no IO scheduler available");
            m_runtime.stop();
            return false;
        }
        
        m_listener = std::make_unique<TcpSocket>(IPType::IPV4);
        
        auto reuse_result = m_listener->option().handleReuseAddr();
        if (!reuse_result) {
            HTTP_LOG_ERROR("failed to set reuse addr: {}", reuse_result.error().message());
            m_runtime.stop();
            return false;
        }
        
        auto nonblock_result = m_listener->option().handleNonBlock();
        if (!nonblock_result) {
            HTTP_LOG_ERROR("failed to set non-block: {}", nonblock_result.error().message());
            m_runtime.stop();
            return false;
        }
        
        Host bind_host(IPType::IPV4, m_config.host, m_config.port);
        auto bind_result = m_listener->bind(bind_host);
        if (!bind_result) {
            HTTP_LOG_ERROR("failed to bind {}:{}: {}", m_config.host, m_config.port, bind_result.error().message());
            m_runtime.stop();
            return false;
        }
        
        auto listen_result = m_listener->listen(m_config.backlog);
        if (!listen_result) {
            HTTP_LOG_ERROR("failed to listen: {}", listen_result.error().message());
            m_runtime.stop();
            return false;
        }
        
        m_running.store(true);
        HTTP_LOG_INFO("H2c server started on {}:{}", m_config.host, m_config.port);
        
        scheduler->spawn(serverLoop());
        
        return true;
    }

    Coroutine serverLoop() {
        while (m_running.load()) {
            Host client_host;
            auto accept_result = co_await m_listener->accept(&client_host);
            
            if (!accept_result) {
                if (m_running.load()) {
                    HTTP_LOG_ERROR("accept failed: {}", accept_result.error().message());
                }
                continue;
            }
            
            HTTP_LOG_INFO("H2c client connected from {}:{}", client_host.ip(), client_host.port());
            
            auto* scheduler = m_runtime.getNextIOScheduler();
            if (!scheduler) {
                HTTP_LOG_ERROR("no IO scheduler available");
                continue;
            }
            
            TcpSocket client_socket(accept_result.value());
            auto nonblock_result = client_socket.option().handleNonBlock();
            if (!nonblock_result) {
                HTTP_LOG_ERROR("failed to set client socket non-block: {}", nonblock_result.error().message());
                continue;
            }
            
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
        conn.localSettings().max_concurrent_streams = m_config.max_concurrent_streams;
        conn.localSettings().initial_window_size = m_config.initial_window_size;
        conn.localSettings().max_frame_size = m_config.max_frame_size;
        conn.localSettings().max_header_list_size = m_config.max_header_list_size;
        conn.localSettings().enable_push = m_config.enable_push ? 1 : 0;

        // 检测协议（Prior Knowledge 或 HTTP/1.1 升级）
        bool detect_success = false;
        co_await detectProtocol(conn, detect_success).wait();
        if (!detect_success) {
            HTTP_LOG_ERROR("protocol detection failed");
            co_await conn.close();
            co_return;
        }

        // 处理 HTTP/2 连接
        co_await handleHttp2Connection(conn).wait();

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
            HTTP_LOG_DEBUG("HTTP/2 Prior Knowledge detected");

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
            HTTP_LOG_DEBUG("HTTP/1.1 request detected, checking for Upgrade");

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
                HTTP_LOG_ERROR("HTTP header too large or incomplete");
                co_return;
            }

            std::string header_str = preface_data.substr(0, header_end + 4);

            // 检查是否包含 Upgrade: h2c
            bool has_upgrade = header_str.find("Upgrade: h2c") != std::string::npos ||
                              header_str.find("upgrade: h2c") != std::string::npos;
            bool has_http2_settings = header_str.find("HTTP2-Settings:") != std::string::npos ||
                                     header_str.find("http2-settings:") != std::string::npos;

            if (has_upgrade && has_http2_settings) {
                HTTP_LOG_DEBUG("HTTP/1.1 Upgrade to h2c detected");

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
                        HTTP_LOG_ERROR("failed to send upgrade response");
                        co_return;
                    }
                    sent += send_result.value();
                }

                HTTP_LOG_DEBUG("sent 101 Switching Protocols response");

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
                        HTTP_LOG_ERROR("failed to receive connection preface");
                        co_return;
                    }
                    auto& bytes = result.value();
                    preface_buf.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                }

                // 验证 Connection Preface
                if (std::memcmp(preface_buf.data(), kHttp2ConnectionPreface.data(), kHttp2ConnectionPrefaceLength) != 0) {
                    HTTP_LOG_ERROR("invalid HTTP/2 connection preface after upgrade");
                    co_return;
                }

                HTTP_LOG_DEBUG("HTTP/2 connection preface verified");

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

        HTTP_LOG_WARN("neither HTTP/2 Prior Knowledge nor HTTP/1.1 Upgrade detected");
        co_return;
    }


    /**
     * @brief 处理 HTTP/2 连接
     */
    Coroutine handleHttp2Connection(Http2ConnImpl<TcpSocket>& conn) {
        bool first_settings_received = false;

        while (!conn.isGoawaySent() && !conn.isGoawayReceived()) {
            auto read_awaitable = conn.readFrame();
            auto frame_result = co_await read_awaitable;

            if (!frame_result) {
                // NoError 表示需要更多数据，继续读取
                if (frame_result.error() == Http2ErrorCode::NoError) {
                    continue;
                }
                HTTP_LOG_ERROR("frame read error: {}", http2ErrorCodeToString(frame_result.error()));
                co_await conn.sendGoaway(frame_result.error());
                break;
            }
            
            auto& frame = *frame_result;
            uint32_t stream_id = frame->streamId();

            HTTP_LOG_DEBUG("received frame: type={}, stream={}, flags=0x{:02x}",
                          http2FrameTypeToString(frame->type()), stream_id, frame->header().flags);
            
            // 处理 CONTINUATION 状态
            if (conn.isExpectingContinuation()) {
                if (frame->type() != Http2FrameType::Continuation ||
                    stream_id != conn.continuationStreamId()) {
                    co_await conn.sendGoaway(Http2ErrorCode::ProtocolError);
                    break;
                }
            }
            
            switch (frame->type()) {
                case Http2FrameType::Settings: {
                    auto* settings = static_cast<Http2SettingsFrame*>(frame.get());
                    if (settings->isAck()) {
                        HTTP_LOG_DEBUG("SETTINGS ACK received");
                    } else {
                        // 应用对端设置
                        conn.peerSettings().applySettings(*settings);
                        conn.encoder().setMaxTableSize(conn.peerSettings().header_table_size);
                        
                        // 发送 ACK
                        auto ack_awaitable = conn.sendSettingsAck();
                        auto ack_result = co_await ack_awaitable;
                        if (!ack_result) {
                            co_await conn.sendGoaway(Http2ErrorCode::InternalError);
                            break;
                        }
                        
                        if (!first_settings_received) {
                            first_settings_received = true;
                            HTTP_LOG_DEBUG("initial SETTINGS exchange completed");
                        }
                    }
                    break;
                }
                
                case Http2FrameType::Headers: {
                    auto* headers = static_cast<Http2HeadersFrame*>(frame.get());
                    
                    if (stream_id == 0) {
                        co_await conn.sendGoaway(Http2ErrorCode::ProtocolError);
                        break;
                    }
                    
                    // 获取或创建流
                    auto stream = conn.getStream(stream_id);
                    if (!stream) {
                        // 新流
                        if (stream_id <= conn.lastPeerStreamId()) {
                            co_await conn.sendGoaway(Http2ErrorCode::ProtocolError);
                            break;
                        }
                        
                        // 检查并发流数量
                        if (conn.streamCount() >= conn.localSettings().max_concurrent_streams) {
                            co_await conn.sendRstStream(stream_id, Http2ErrorCode::RefusedStream);
                            continue;
                        }
                        
                        stream = conn.createStream(stream_id);
                        conn.setLastPeerStreamId(stream_id);
                    }
                    
                    // 累积头部块
                    stream->appendHeaderBlock(headers->headerBlock());
                    
                    if (headers->isEndHeaders()) {
                        // 解码头部
                        auto decode_result = conn.decoder().decode(stream->headerBlock());
                        if (!decode_result) {
                            co_await conn.sendGoaway(Http2ErrorCode::CompressionError);
                            break;
                        }
                        
                        stream->clearHeaderBlock();
                        stream->setEndHeadersReceived();
                        conn.setExpectingContinuation(false);
                        
                        // 解析请求
                        Http2Request request;
                        for (const auto& field : *decode_result) {
                            if (field.name == ":method") {
                                request.method = field.value;
                            } else if (field.name == ":scheme") {
                                request.scheme = field.value;
                            } else if (field.name == ":authority") {
                                request.authority = field.value;
                            } else if (field.name == ":path") {
                                request.path = field.value;
                            } else {
                                request.headers.push_back(field);
                            }
                        }
                        
                        stream->request() = request;
                        stream->onHeadersReceived(headers->isEndStream());
                        
                        // 如果请求完整，调用处理器
                        if (headers->isEndStream()) {
                            m_runtime.getNextIOScheduler()->spawn(
                                m_handler(conn, stream, std::move(request)));
                        }
                    } else {
                        // 等待 CONTINUATION
                        conn.setExpectingContinuation(true, stream_id);
                    }
                    break;
                }
                
                case Http2FrameType::Continuation: {
                    auto* continuation = static_cast<Http2ContinuationFrame*>(frame.get());
                    auto stream = conn.getStream(stream_id);
                    
                    if (!stream) {
                        co_await conn.sendGoaway(Http2ErrorCode::ProtocolError);
                        break;
                    }
                    
                    stream->appendHeaderBlock(continuation->headerBlock());
                    
                    if (continuation->isEndHeaders()) {
                        // 解码头部
                        auto decode_result = conn.decoder().decode(stream->headerBlock());
                        if (!decode_result) {
                            co_await conn.sendGoaway(Http2ErrorCode::CompressionError);
                            break;
                        }
                        
                        stream->clearHeaderBlock();
                        stream->setEndHeadersReceived();
                        conn.setExpectingContinuation(false);
                        
                        // 解析请求
                        Http2Request request;
                        for (const auto& field : *decode_result) {
                            if (field.name == ":method") {
                                request.method = field.value;
                            } else if (field.name == ":scheme") {
                                request.scheme = field.value;
                            } else if (field.name == ":authority") {
                                request.authority = field.value;
                            } else if (field.name == ":path") {
                                request.path = field.value;
                            } else {
                                request.headers.push_back(field);
                            }
                        }
                        
                        stream->request() = request;
                        
                        // 如果之前的 HEADERS 有 END_STREAM，调用处理器
                        if (stream->isEndStreamReceived()) {
                            m_runtime.getNextIOScheduler()->spawn(
                                m_handler(conn, stream, std::move(request)));
                        }
                    }
                    break;
                }


                case Http2FrameType::Data: {
                    auto* data = static_cast<Http2DataFrame*>(frame.get());
                    
                    if (stream_id == 0) {
                        co_await conn.sendGoaway(Http2ErrorCode::ProtocolError);
                        break;
                    }
                    
                    auto stream = conn.getStream(stream_id);
                    if (!stream) {
                        co_await conn.sendRstStream(stream_id, Http2ErrorCode::StreamClosed);
                        continue;
                    }
                    
                    if (!stream->canReceiveData()) {
                        co_await conn.sendRstStream(stream_id, Http2ErrorCode::StreamClosed);
                        continue;
                    }
                    
                    // 累积数据
                    stream->appendData(data->data());
                    
                    // 更新流量控制窗口
                    conn.adjustConnRecvWindow(-static_cast<int32_t>(data->data().size()));
                    stream->adjustRecvWindow(-static_cast<int32_t>(data->data().size()));
                    
                    // 发送 WINDOW_UPDATE（简化：每次都发送）
                    if (conn.connRecvWindow() < static_cast<int32_t>(kDefaultInitialWindowSize / 2)) {
                        uint32_t increment = kDefaultInitialWindowSize - conn.connRecvWindow();
                        co_await conn.sendWindowUpdate(0, increment);
                        conn.adjustConnRecvWindow(increment);
                    }
                    
                    if (stream->recvWindow() < static_cast<int32_t>(kDefaultInitialWindowSize / 2)) {
                        uint32_t increment = kDefaultInitialWindowSize - stream->recvWindow();
                        co_await conn.sendWindowUpdate(stream_id, increment);
                        stream->adjustRecvWindow(increment);
                    }
                    
                    stream->onDataReceived(data->isEndStream());
                    
                    // 如果请求完整，调用处理器
                    if (data->isEndStream() && stream->isEndHeadersReceived()) {
                        Http2Request request = stream->request();
                        request.body = stream->request().body;
                        m_runtime.getNextIOScheduler()->spawn(
                            m_handler(conn, stream, std::move(request)));
                    }
                    break;
                }
                
                case Http2FrameType::Ping: {
                    auto* ping = static_cast<Http2PingFrame*>(frame.get());
                    
                    if (stream_id != 0) {
                        co_await conn.sendGoaway(Http2ErrorCode::ProtocolError);
                        break;
                    }
                    
                    if (!ping->isAck()) {
                        // 回复 PING ACK
                        co_await conn.sendPing(ping->opaqueData(), true);
                    }
                    break;
                }
                
                case Http2FrameType::WindowUpdate: {
                    auto* window_update = static_cast<Http2WindowUpdateFrame*>(frame.get());
                    uint32_t increment = window_update->windowSizeIncrement();
                    
                    if (increment == 0) {
                        if (stream_id == 0) {
                            co_await conn.sendGoaway(Http2ErrorCode::ProtocolError);
                        } else {
                            co_await conn.sendRstStream(stream_id, Http2ErrorCode::ProtocolError);
                        }
                        break;
                    }
                    
                    if (stream_id == 0) {
                        conn.adjustConnSendWindow(increment);
                    } else {
                        auto stream = conn.getStream(stream_id);
                        if (stream) {
                            stream->adjustSendWindow(increment);
                        }
                    }
                    break;
                }
                
                case Http2FrameType::RstStream: {
                    auto* rst = static_cast<Http2RstStreamFrame*>(frame.get());
                    
                    if (stream_id == 0) {
                        co_await conn.sendGoaway(Http2ErrorCode::ProtocolError);
                        break;
                    }
                    
                    auto stream = conn.getStream(stream_id);
                    if (stream) {
                        stream->onRstStreamReceived();
                        HTTP_LOG_DEBUG("stream {} reset with error: {}", 
                                      stream_id, http2ErrorCodeToString(rst->errorCode()));
                    }
                    break;
                }
                
                case Http2FrameType::GoAway: {
                    auto* goaway = static_cast<Http2GoAwayFrame*>(frame.get());
                    conn.setGoawayReceived();
                    HTTP_LOG_INFO("GOAWAY received: last_stream={}, error={}, debug={}",
                                 goaway->lastStreamId(),
                                 http2ErrorCodeToString(goaway->errorCode()),
                                 goaway->debugData());
                    break;
                }
                
                case Http2FrameType::Priority: {
                    // 优先级帧，可以忽略
                    HTTP_LOG_DEBUG("PRIORITY frame received (ignored)");
                    break;
                }
                
                case Http2FrameType::PushPromise: {
                    // 客户端不应该发送 PUSH_PROMISE
                    co_await conn.sendGoaway(Http2ErrorCode::ProtocolError);
                    break;
                }
                
                default:
                    HTTP_LOG_WARN("unknown frame type: {}", static_cast<int>(frame->type()));
                    break;
            }
        }
        
        co_return;
    }

private:
    Runtime m_runtime;
    H2cServerConfig m_config;
    Http2RequestHandler m_handler;
    std::unique_ptr<TcpSocket> m_listener;
    std::atomic<bool> m_running;
};

} // namespace galay::http2

#endif // GALAY_HTTP2_SERVER_H
