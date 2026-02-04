#ifndef GALAY_HTTP2_CONN_H
#define GALAY_HTTP2_CONN_H

#include "Http2Stream.h"
#include "galay-http/protoc/http2/Http2Base.h"
#include "galay-http/protoc/http2/Http2Frame.h"
#include "galay-http/protoc/http2/Http2Hpack.h"
#include "galay-http/protoc/http2/Http2Error.h"
#include "galay-http/kernel/http/HttpLog.h"
#include "galay-http/kernel/http/HttpConn.h"
#include "galay-kernel/common/Buffer.h"
#include "galay-kernel/kernel/Awaitable.h"
#include "galay-kernel/kernel/Timeout.hpp"
#include "galay-kernel/async/TcpSocket.h"
#include <unordered_map>
#include <memory>
#include <expected>
#include <functional>
#include <cstring>

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/SslSocket.h"
#endif

namespace galay::http2
{

using namespace galay::kernel;

// 类型特征：检测是否是 SslSocket
template<typename T>
struct is_ssl_socket : std::false_type {};

#ifdef GALAY_HTTP_SSL_ENABLED
template<>
struct is_ssl_socket<galay::ssl::SslSocket> : std::true_type {};
#endif

template<typename T>
inline constexpr bool is_ssl_socket_v = is_ssl_socket<T>::value;

/**
 * @brief HTTP/2 连接设置
 */
struct Http2Settings
{
    uint32_t header_table_size = kDefaultHeaderTableSize;
    uint32_t enable_push = kDefaultEnablePush;
    uint32_t max_concurrent_streams = kDefaultMaxConcurrentStreams;
    uint32_t initial_window_size = kDefaultInitialWindowSize;
    uint32_t max_frame_size = kDefaultMaxFrameSize;
    uint32_t max_header_list_size = kDefaultMaxHeaderListSize;
    
    void applySettings(const Http2SettingsFrame& frame) {
        for (const auto& setting : frame.settings()) {
            switch (setting.id) {
                case Http2SettingsId::HeaderTableSize:
                    header_table_size = setting.value;
                    break;
                case Http2SettingsId::EnablePush:
                    enable_push = setting.value;
                    break;
                case Http2SettingsId::MaxConcurrentStreams:
                    max_concurrent_streams = setting.value;
                    break;
                case Http2SettingsId::InitialWindowSize:
                    initial_window_size = setting.value;
                    break;
                case Http2SettingsId::MaxFrameSize:
                    max_frame_size = setting.value;
                    break;
                case Http2SettingsId::MaxHeaderListSize:
                    max_header_list_size = setting.value;
                    break;
            }
        }
    }
    
    Http2SettingsFrame toFrame() const {
        Http2SettingsFrame frame;
        frame.addSetting(Http2SettingsId::HeaderTableSize, header_table_size);
        frame.addSetting(Http2SettingsId::EnablePush, enable_push);
        frame.addSetting(Http2SettingsId::MaxConcurrentStreams, max_concurrent_streams);
        frame.addSetting(Http2SettingsId::InitialWindowSize, initial_window_size);
        frame.addSetting(Http2SettingsId::MaxFrameSize, max_frame_size);
        frame.addSetting(Http2SettingsId::MaxHeaderListSize, max_header_list_size);
        return frame;
    }
};

// 前向声明
template<typename SocketType>
class Http2ConnImpl;

/**
 * @brief HTTP/2 帧读取等待体 - TcpSocket 版本（使用 readv）
 */
template<typename SocketType, bool IsSsl = is_ssl_socket_v<SocketType>>
class Http2ReadFrameAwaitableImpl;

// TcpSocket 特化版本（使用 readv）
template<typename SocketType>
class Http2ReadFrameAwaitableImpl<SocketType, false> : public TimeoutSupport<Http2ReadFrameAwaitableImpl<SocketType, false>>
{
public:
    using ReadvAwaitableType = decltype(std::declval<SocketType>().readv(std::declval<std::vector<iovec>>()));

    Http2ReadFrameAwaitableImpl(RingBuffer& ring_buffer, Http2Settings& peer_settings, SocketType& socket)
        : m_ring_buffer(ring_buffer)
        , m_peer_settings(peer_settings)
        , m_socket(socket)
        , m_has_buffered_frame(false)
    {
        // 检查 RingBuffer 中是否已经有完整的帧
        checkBufferedFrame();
    }

    bool await_ready() const noexcept {
        return m_has_buffered_frame;
    }

    template<typename Handle>
    auto await_suspend(Handle handle) {
        if (!m_readv_awaitable) {
            m_readv_awaitable.emplace(m_socket.readv(m_ring_buffer.getWriteIovecs()));
        }
        return m_readv_awaitable->await_suspend(handle);
    }

    std::expected<Http2Frame::uptr, Http2ErrorCode> await_resume() {
        // 如果已经有缓冲的帧，直接解析
        if (m_has_buffered_frame) {
            return parseFrameFromBuffer();
        }

        auto readv_result = m_readv_awaitable->await_resume();
        m_readv_awaitable.reset();

        if (!readv_result) {
            HTTP_LOG_DEBUG("[readv] [fail] [{}]", readv_result.error().message());
            return std::unexpected(Http2ErrorCode::ProtocolError);
        }

        ssize_t bytes_read = readv_result.value();
        if (bytes_read == 0) {
            HTTP_LOG_DEBUG("[conn] [closed]");
            return std::unexpected(Http2ErrorCode::ProtocolError);
        }

        m_ring_buffer.produce(bytes_read);

        return parseFrameFromBuffer();
    }

private:
    void checkBufferedFrame() {
        if (m_ring_buffer.readable() < kHttp2FrameHeaderLength) {
            m_has_buffered_frame = false;
            return;
        }

        auto read_iovecs = m_ring_buffer.getReadIovecs();
        if (read_iovecs.empty()) {
            m_has_buffered_frame = false;
            return;
        }

        uint8_t header_buf[kHttp2FrameHeaderLength];
        size_t copied = 0;
        for (const auto& iov : read_iovecs) {
            size_t to_copy = std::min(iov.iov_len, kHttp2FrameHeaderLength - copied);
            std::memcpy(header_buf + copied, iov.iov_base, to_copy);
            copied += to_copy;
            if (copied >= kHttp2FrameHeaderLength) break;
        }

        Http2FrameHeader header = Http2FrameHeader::deserialize(header_buf);
        size_t total_frame_size = kHttp2FrameHeaderLength + header.length;

        m_has_buffered_frame = (m_ring_buffer.readable() >= total_frame_size);
    }

    std::expected<Http2Frame::uptr, Http2ErrorCode> parseFrameFromBuffer() {
        if (m_ring_buffer.readable() < kHttp2FrameHeaderLength) {
            return std::unexpected(Http2ErrorCode::NoError);
        }

        auto read_iovecs = m_ring_buffer.getReadIovecs();
        if (read_iovecs.empty()) {
            return std::unexpected(Http2ErrorCode::NoError);
        }

        uint8_t header_buf[kHttp2FrameHeaderLength];
        size_t copied = 0;
        for (const auto& iov : read_iovecs) {
            size_t to_copy = std::min(iov.iov_len, kHttp2FrameHeaderLength - copied);
            std::memcpy(header_buf + copied, iov.iov_base, to_copy);
            copied += to_copy;
            if (copied >= kHttp2FrameHeaderLength) break;
        }

        Http2FrameHeader header = Http2FrameHeader::deserialize(header_buf);

        if (header.length > m_peer_settings.max_frame_size) {
            return std::unexpected(Http2ErrorCode::FrameSizeError);
        }

        size_t total_frame_size = kHttp2FrameHeaderLength + header.length;

        if (m_ring_buffer.readable() < total_frame_size) {
            return std::unexpected(Http2ErrorCode::NoError);
        }

        std::vector<uint8_t> frame_buf(total_frame_size);
        copied = 0;
        for (const auto& iov : read_iovecs) {
            size_t to_copy = std::min(iov.iov_len, total_frame_size - copied);
            std::memcpy(frame_buf.data() + copied, iov.iov_base, to_copy);
            copied += to_copy;
            if (copied >= total_frame_size) break;
        }

        auto frame_result = Http2FrameParser::parseFrame(frame_buf.data(), total_frame_size);

        if (frame_result) {
            m_ring_buffer.consume(total_frame_size);
        }

        return frame_result;
    }

    RingBuffer& m_ring_buffer;
    Http2Settings& m_peer_settings;
    SocketType& m_socket;
    std::optional<ReadvAwaitableType> m_readv_awaitable;
    bool m_has_buffered_frame;
};

#ifdef GALAY_HTTP_SSL_ENABLED
// SslSocket 特化版本（使用 recv）
template<typename SocketType>
class Http2ReadFrameAwaitableImpl<SocketType, true> : public TimeoutSupport<Http2ReadFrameAwaitableImpl<SocketType, true>>
{
public:
    using RecvAwaitableType = decltype(std::declval<SocketType>().recv(std::declval<char*>(), std::declval<size_t>()));
    
    Http2ReadFrameAwaitableImpl(RingBuffer& ring_buffer, Http2Settings& peer_settings, SocketType& socket)
        : m_ring_buffer(ring_buffer)
        , m_peer_settings(peer_settings)
        , m_socket(socket)
    {
    }
    
    bool await_ready() const noexcept { return false; }
    
    template<typename Handle>
    auto await_suspend(Handle handle) {
        if (!m_recv_awaitable) {
            auto write_iovecs = m_ring_buffer.getWriteIovecs();
            if (!write_iovecs.empty()) {
                m_recv_awaitable.emplace(m_socket.recv(
                    static_cast<char*>(write_iovecs[0].iov_base),
                    write_iovecs[0].iov_len));
            }
        }
        return m_recv_awaitable->await_suspend(handle);
    }
    
    std::expected<Http2Frame::uptr, Http2ErrorCode> await_resume() {
        auto recv_result = m_recv_awaitable->await_resume();
        m_recv_awaitable.reset();
        
        if (!recv_result) {
            HTTP_LOG_DEBUG("[ssl] [recv-fail] [{}]", recv_result.error().message());
            return std::unexpected(Http2ErrorCode::ProtocolError);
        }
        
        ssize_t bytes_read = static_cast<ssize_t>(recv_result.value().size());
        if (bytes_read == 0) {
            HTTP_LOG_DEBUG("[ssl] [closed]");
            return std::unexpected(Http2ErrorCode::ProtocolError);
        }
        
        m_ring_buffer.produce(bytes_read);
        
        if (m_ring_buffer.readable() < kHttp2FrameHeaderLength) {
            return std::unexpected(Http2ErrorCode::NoError);
        }
        
        auto read_iovecs = m_ring_buffer.getReadIovecs();
        if (read_iovecs.empty()) {
            return std::unexpected(Http2ErrorCode::NoError);
        }
        
        uint8_t header_buf[kHttp2FrameHeaderLength];
        size_t copied = 0;
        for (const auto& iov : read_iovecs) {
            size_t to_copy = std::min(iov.iov_len, kHttp2FrameHeaderLength - copied);
            std::memcpy(header_buf + copied, iov.iov_base, to_copy);
            copied += to_copy;
            if (copied >= kHttp2FrameHeaderLength) break;
        }
        
        Http2FrameHeader header = Http2FrameHeader::deserialize(header_buf);
        
        if (header.length > m_peer_settings.max_frame_size) {
            return std::unexpected(Http2ErrorCode::FrameSizeError);
        }
        
        size_t total_frame_size = kHttp2FrameHeaderLength + header.length;
        
        if (m_ring_buffer.readable() < total_frame_size) {
            return std::unexpected(Http2ErrorCode::NoError);
        }
        
        std::vector<uint8_t> frame_buf(total_frame_size);
        copied = 0;
        for (const auto& iov : read_iovecs) {
            size_t to_copy = std::min(iov.iov_len, total_frame_size - copied);
            std::memcpy(frame_buf.data() + copied, iov.iov_base, to_copy);
            copied += to_copy;
            if (copied >= total_frame_size) break;
        }
        
        auto frame_result = Http2FrameParser::parseFrame(frame_buf.data(), total_frame_size);
        
        if (frame_result) {
            m_ring_buffer.consume(total_frame_size);
        }
        
        return frame_result;
    }

private:
    RingBuffer& m_ring_buffer;
    Http2Settings& m_peer_settings;
    SocketType& m_socket;
    std::optional<RecvAwaitableType> m_recv_awaitable;
};
#endif

/**
 * @brief HTTP/2 帧写入等待体 - TcpSocket 版本（使用 writev）
 */
template<typename SocketType, bool IsSsl = is_ssl_socket_v<SocketType>>
class Http2WriteFrameAwaitableImpl;

// TcpSocket 特化版本
template<typename SocketType>
class Http2WriteFrameAwaitableImpl<SocketType, false> : public TimeoutSupport<Http2WriteFrameAwaitableImpl<SocketType, false>>
{
public:
    using SendAwaitableType = decltype(std::declval<SocketType>().send(std::declval<const char*>(), std::declval<size_t>()));
    
    Http2WriteFrameAwaitableImpl(SocketType& socket, std::string data)
        : m_socket(socket)
        , m_data(std::move(data))
        , m_offset(0)
    {
    }
    
    bool await_ready() const noexcept { return false; }
    
    template<typename Handle>
    auto await_suspend(Handle handle) {
        if (!m_send_awaitable) {
            m_send_awaitable.emplace(m_socket.send(m_data.data() + m_offset, m_data.size() - m_offset));
        }
        return m_send_awaitable->await_suspend(handle);
    }
    
    std::expected<bool, Http2ErrorCode> await_resume() {
        auto send_result = m_send_awaitable->await_resume();
        m_send_awaitable.reset();

        if (!send_result) {
            HTTP_LOG_ERROR("[frame] [send-fail] [{}]", send_result.error().message());
            return std::unexpected(Http2ErrorCode::InternalError);
        }

        m_offset += send_result.value();
        HTTP_LOG_DEBUG("[frame] [sent] [bytes={}] [total={}/{}]",
                      send_result.value(), m_offset, m_data.size());

        if (m_offset >= m_data.size()) {
            return true;  // 发送完成
        }

        return false;  // 需要继续发送
    }

private:
    SocketType& m_socket;
    std::string m_data;
    size_t m_offset;
    std::optional<SendAwaitableType> m_send_awaitable;
};

#ifdef GALAY_HTTP_SSL_ENABLED
// SslSocket 特化版本
template<typename SocketType>
class Http2WriteFrameAwaitableImpl<SocketType, true> : public TimeoutSupport<Http2WriteFrameAwaitableImpl<SocketType, true>>
{
public:
    using SendAwaitableType = decltype(std::declval<SocketType>().send(std::declval<const char*>(), std::declval<size_t>()));
    
    Http2WriteFrameAwaitableImpl(SocketType& socket, std::string data)
        : m_socket(socket)
        , m_data(std::move(data))
        , m_offset(0)
    {
    }
    
    bool await_ready() const noexcept { return false; }
    
    template<typename Handle>
    auto await_suspend(Handle handle) {
        if (!m_send_awaitable) {
            m_send_awaitable.emplace(m_socket.send(m_data.data() + m_offset, m_data.size() - m_offset));
        }
        return m_send_awaitable->await_suspend(handle);
    }
    
    std::expected<bool, Http2ErrorCode> await_resume() {
        auto send_result = m_send_awaitable->await_resume();
        m_send_awaitable.reset();
        
        if (!send_result) {
            HTTP_LOG_DEBUG("[ssl] [send-fail] [{}]", send_result.error().message());
            return std::unexpected(Http2ErrorCode::InternalError);
        }
        
        m_offset += send_result.value();
        
        if (m_offset >= m_data.size()) {
            return true;
        }
        
        return false;
    }

private:
    SocketType& m_socket;
    std::string m_data;
    size_t m_offset;
    std::optional<SendAwaitableType> m_send_awaitable;
};
#endif


/**
 * @brief HTTP/2 连接模板类
 */
template<typename SocketType>
class Http2ConnImpl
{
public:
    /**
     * @brief 从 Socket 构造（Prior Knowledge 模式）
     */
    Http2ConnImpl(SocketType&& socket)
        : m_socket(std::move(socket))
        , m_ring_buffer(65536)  // 64KB buffer
        , m_last_peer_stream_id(0)
        , m_last_local_stream_id(0)
        , m_conn_send_window(kDefaultInitialWindowSize)
        , m_conn_recv_window(kDefaultInitialWindowSize)
        , m_goaway_sent(false)
        , m_goaway_received(false)
        , m_expecting_continuation(false)
        , m_continuation_stream_id(0)
    {
    }

    /**
     * @brief 从 HttpConn 升级构造（h2c Upgrade 模式）
     * @details 类似 WebSocket 从 HTTP/1.1 升级的方式
     */
    Http2ConnImpl(galay::http::HttpConnImpl<SocketType>&& http_conn)
        : m_socket(std::move(http_conn.m_socket))
        , m_ring_buffer(std::move(http_conn.m_ring_buffer))
        , m_last_peer_stream_id(0)
        , m_last_local_stream_id(0)
        , m_conn_send_window(kDefaultInitialWindowSize)
        , m_conn_recv_window(kDefaultInitialWindowSize)
        , m_goaway_sent(false)
        , m_goaway_received(false)
        , m_expecting_continuation(false)
        , m_continuation_stream_id(0)
    {
        // 升级后需要扩展 buffer 大小以适应 HTTP/2
        if (m_ring_buffer.capacity() < 65536) {
            // 保留已有数据，扩展容量
            RingBuffer new_buffer(65536);
            // 复制已有数据到新 buffer
            auto read_iovecs = m_ring_buffer.getReadIovecs();
            for (const auto& iov : read_iovecs) {
                auto write_iovecs = new_buffer.getWriteIovecs();
                if (!write_iovecs.empty()) {
                    size_t to_copy = std::min(iov.iov_len, write_iovecs[0].iov_len);
                    std::memcpy(write_iovecs[0].iov_base, iov.iov_base, to_copy);
                    new_buffer.produce(to_copy);
                }
            }
            m_ring_buffer = std::move(new_buffer);
        }
    }

    /**
     * @brief 从 Socket 和 RingBuffer 构造
     */
    Http2ConnImpl(SocketType&& socket, RingBuffer&& ring_buffer)
        : m_socket(std::move(socket))
        , m_ring_buffer(std::move(ring_buffer))
        , m_last_peer_stream_id(0)
        , m_last_local_stream_id(0)
        , m_conn_send_window(kDefaultInitialWindowSize)
        , m_conn_recv_window(kDefaultInitialWindowSize)
        , m_goaway_sent(false)
        , m_goaway_received(false)
        , m_expecting_continuation(false)
        , m_continuation_stream_id(0)
    {
    }

    ~Http2ConnImpl() = default;
    
    // 禁用拷贝
    Http2ConnImpl(const Http2ConnImpl&) = delete;
    Http2ConnImpl& operator=(const Http2ConnImpl&) = delete;
    
    // 启用移动
    Http2ConnImpl(Http2ConnImpl&&) = default;
    Http2ConnImpl& operator=(Http2ConnImpl&&) = default;
    
    // 获取 socket
    SocketType& socket() { return m_socket; }
    
    // 获取本地/对端设置
    Http2Settings& localSettings() { return m_local_settings; }
    Http2Settings& peerSettings() { return m_peer_settings; }
    
    // HPACK 编解码器
    HpackEncoder& encoder() { return m_encoder; }
    HpackDecoder& decoder() { return m_decoder; }
    
    // 流管理
    Http2Stream::ptr getStream(uint32_t stream_id) {
        auto it = m_streams.find(stream_id);
        return it != m_streams.end() ? it->second : nullptr;
    }
    
    Http2Stream::ptr createStream(uint32_t stream_id) {
        auto stream = std::make_shared<Http2Stream>(stream_id);
        m_streams[stream_id] = stream;
        return stream;
    }
    
    void removeStream(uint32_t stream_id) {
        m_streams.erase(stream_id);
    }
    
    size_t streamCount() const { return m_streams.size(); }
    
    // 获取下一个本地流 ID（服务器使用偶数）
    uint32_t nextLocalStreamId() {
        if (m_last_local_stream_id == 0) {
            m_last_local_stream_id = 2;
        } else {
            m_last_local_stream_id += 2;
        }
        return m_last_local_stream_id;
    }
    
    // 连接级流量控制
    int32_t connSendWindow() const { return m_conn_send_window; }
    int32_t connRecvWindow() const { return m_conn_recv_window; }
    void adjustConnSendWindow(int32_t delta) { m_conn_send_window += delta; }
    void adjustConnRecvWindow(int32_t delta) { m_conn_recv_window += delta; }
    
    // GOAWAY 状态
    bool isGoawaySent() const { return m_goaway_sent; }
    bool isGoawayReceived() const { return m_goaway_received; }
    void setGoawaySent() { m_goaway_sent = true; }
    void setGoawayReceived() { m_goaway_received = true; }
    
    // 最后处理的流 ID
    uint32_t lastPeerStreamId() const { return m_last_peer_stream_id; }
    void setLastPeerStreamId(uint32_t id) { m_last_peer_stream_id = id; }
    
    // CONTINUATION 状态
    bool isExpectingContinuation() const { return m_expecting_continuation; }
    uint32_t continuationStreamId() const { return m_continuation_stream_id; }
    void setExpectingContinuation(bool expecting, uint32_t stream_id = 0) {
        m_expecting_continuation = expecting;
        m_continuation_stream_id = stream_id;
    }
    
    // 关闭连接
    auto close() {
        return m_socket.close();
    }

    /**
     * @brief 将数据放入接收缓冲区
     * @param data 数据指针
     * @param len 数据长度
     */
    void feedData(const char* data, size_t len) {
        auto write_iovecs = m_ring_buffer.getWriteIovecs();
        size_t copied = 0;
        for (const auto& iov : write_iovecs) {
            size_t to_copy = std::min(iov.iov_len, len - copied);
            std::memcpy(iov.iov_base, data + copied, to_copy);
            copied += to_copy;
            if (copied >= len) break;
        }
        m_ring_buffer.produce(copied);
    }

    // ==================== 帧读写（返回 Awaitable） ====================
    
    /**
     * @brief 获取帧读取等待体
     */
    Http2ReadFrameAwaitableImpl<SocketType> readFrame() {
        return Http2ReadFrameAwaitableImpl<SocketType>(m_ring_buffer, m_peer_settings, m_socket);
    }
    
    /**
     * @brief 获取帧写入等待体
     */
    Http2WriteFrameAwaitableImpl<SocketType> writeFrame(const Http2Frame& frame) {
        return Http2WriteFrameAwaitableImpl<SocketType>(m_socket, frame.serialize());
    }
    
    /**
     * @brief 获取原始数据写入等待体
     */
    Http2WriteFrameAwaitableImpl<SocketType> writeRaw(std::string data) {
        return Http2WriteFrameAwaitableImpl<SocketType>(m_socket, std::move(data));
    }

    // ==================== 便捷方法 ====================
    
    /**
     * @brief 发送 SETTINGS 帧
     */
    Http2WriteFrameAwaitableImpl<SocketType> sendSettings() {
        auto frame = m_local_settings.toFrame();
        return writeFrame(frame);
    }
    
    /**
     * @brief 发送 SETTINGS ACK
     */
    Http2WriteFrameAwaitableImpl<SocketType> sendSettingsAck() {
        Http2SettingsFrame frame;
        frame.setAck(true);
        return writeFrame(frame);
    }
    
    /**
     * @brief 发送 PING
     */
    Http2WriteFrameAwaitableImpl<SocketType> sendPing(const uint8_t* data, bool ack = false) {
        Http2PingFrame frame;
        frame.setOpaqueData(data);
        frame.setAck(ack);
        return writeFrame(frame);
    }
    
    /**
     * @brief 发送 GOAWAY
     */
    Http2WriteFrameAwaitableImpl<SocketType> sendGoaway(Http2ErrorCode error, const std::string& debug = "") {
        Http2GoAwayFrame frame;
        frame.setLastStreamId(m_last_peer_stream_id);
        frame.setErrorCode(error);
        frame.setDebugData(debug);
        m_goaway_sent = true;
        return writeFrame(frame);
    }
    
    /**
     * @brief 发送 RST_STREAM
     */
    Http2WriteFrameAwaitableImpl<SocketType> sendRstStream(uint32_t stream_id, Http2ErrorCode error) {
        Http2RstStreamFrame frame;
        frame.header().stream_id = stream_id;
        frame.setErrorCode(error);
        
        auto stream = getStream(stream_id);
        if (stream) {
            stream->onRstStreamSent();
        }
        
        return writeFrame(frame);
    }
    
    /**
     * @brief 发送 WINDOW_UPDATE
     */
    Http2WriteFrameAwaitableImpl<SocketType> sendWindowUpdate(uint32_t stream_id, uint32_t increment) {
        Http2WindowUpdateFrame frame;
        frame.header().stream_id = stream_id;
        frame.setWindowSizeIncrement(increment);
        return writeFrame(frame);
    }
    
    /**
     * @brief 发送 HEADERS 帧
     */
    Http2WriteFrameAwaitableImpl<SocketType> sendHeaders(
        uint32_t stream_id, 
        const std::vector<Http2HeaderField>& headers,
        bool end_stream = false,
        bool end_headers = true)
    {
        std::string header_block = m_encoder.encode(headers);
        
        Http2HeadersFrame frame;
        frame.header().stream_id = stream_id;
        frame.setHeaderBlock(std::move(header_block));
        frame.setEndStream(end_stream);
        frame.setEndHeaders(end_headers);
        
        auto stream = getStream(stream_id);
        if (stream) {
            stream->onHeadersSent(end_stream);
        }
        
        return writeFrame(frame);
    }
    
    /**
     * @brief 发送 DATA 帧（单帧）
     */
    Http2WriteFrameAwaitableImpl<SocketType> sendDataFrame(
        uint32_t stream_id,
        const std::string& data,
        bool end_stream = false)
    {
        Http2DataFrame frame;
        frame.header().stream_id = stream_id;
        frame.setData(data);
        frame.setEndStream(end_stream);
        
        auto stream = getStream(stream_id);
        if (stream) {
            m_conn_send_window -= data.size();
            stream->adjustSendWindow(-static_cast<int32_t>(data.size()));
            if (end_stream) {
                stream->onDataSent(true);
            }
        }
        
        return writeFrame(frame);
    }
    
    /**
     * @brief 发送 PUSH_PROMISE 帧
     */
    Http2WriteFrameAwaitableImpl<SocketType> sendPushPromise(
        uint32_t stream_id,
        uint32_t promised_stream_id,
        const std::vector<Http2HeaderField>& headers)
    {
        std::string header_block = m_encoder.encode(headers);
        
        Http2PushPromiseFrame frame;
        frame.header().stream_id = stream_id;
        frame.setPromisedStreamId(promised_stream_id);
        frame.setHeaderBlock(std::move(header_block));
        frame.setEndHeaders(true);
        
        return writeFrame(frame);
    }
    
    /**
     * @brief 创建推送流并准备 PUSH_PROMISE
     * @return {promised_stream_id, awaitable} 如果推送被禁用返回 {0, nullopt}
     */
    std::pair<uint32_t, std::optional<Http2WriteFrameAwaitableImpl<SocketType>>> preparePushPromise(
        uint32_t stream_id,
        const std::string& method,
        const std::string& path,
        const std::string& authority,
        const std::string& scheme = "http")
    {
        if (!m_peer_settings.enable_push) {
            return {0, std::nullopt};
        }
        
        uint32_t promised_stream_id = nextLocalStreamId();
        
        std::vector<Http2HeaderField> headers;
        headers.push_back({":method", method});
        headers.push_back({":path", path});
        headers.push_back({":authority", authority});
        headers.push_back({":scheme", scheme});
        
        // 创建推送流
        auto push_stream = createStream(promised_stream_id);
        push_stream->setState(Http2StreamState::ReservedLocal);
        
        return {promised_stream_id, sendPushPromise(stream_id, promised_stream_id, headers)};
    }

private:
    SocketType m_socket;
    RingBuffer m_ring_buffer;
    
    // 连接设置
    Http2Settings m_local_settings;
    Http2Settings m_peer_settings;
    
    // 流管理
    std::unordered_map<uint32_t, Http2Stream::ptr> m_streams;
    uint32_t m_last_peer_stream_id;
    uint32_t m_last_local_stream_id;
    
    // HPACK 编解码器
    HpackEncoder m_encoder;
    HpackDecoder m_decoder;
    
    // 连接级流量控制
    int32_t m_conn_send_window;
    int32_t m_conn_recv_window;
    
    // 连接状态
    bool m_goaway_sent;
    bool m_goaway_received;
    
    // CONTINUATION 状态
    bool m_expecting_continuation;
    uint32_t m_continuation_stream_id;
};

// 类型别名
using Http2Conn = Http2ConnImpl<galay::async::TcpSocket>;
using Http2ReadFrameAwaitable = Http2ReadFrameAwaitableImpl<galay::async::TcpSocket>;
using Http2WriteFrameAwaitable = Http2WriteFrameAwaitableImpl<galay::async::TcpSocket>;

#ifdef GALAY_HTTP_SSL_ENABLED
using Http2sConn = Http2ConnImpl<galay::ssl::SslSocket>;
using Http2sReadFrameAwaitable = Http2ReadFrameAwaitableImpl<galay::ssl::SslSocket>;
using Http2sWriteFrameAwaitable = Http2WriteFrameAwaitableImpl<galay::ssl::SslSocket>;
#endif

} // namespace galay::http2

#endif // GALAY_HTTP2_CONN_H
