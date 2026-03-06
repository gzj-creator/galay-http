#ifndef GALAY_HTTP2_CONN_H
#define GALAY_HTTP2_CONN_H

#include "Http2Stream.h"
#include "Http2ConnectionCore.h"
#include "galay-http/protoc/http2/Http2Base.h"
#include "galay-http/protoc/http2/Http2Frame.h"
#include "galay-http/protoc/http2/Http2Hpack.h"
#include "galay-http/protoc/http2/Http2Error.h"
#include "galay-http/kernel/http/HttpLog.h"
#include "galay-http/kernel/http/HttpConn.h"
#include "galay-kernel/common/Buffer.h"
#include "galay-kernel/common/Error.h"
#include "galay-kernel/kernel/Awaitable.h"
#include "galay-kernel/kernel/Timeout.hpp"
#include "galay-kernel/async/TcpSocket.h"
#include <unordered_map>
#include <memory>
#include <expected>
#include <functional>
#include <cstring>
#include <chrono>

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/async/SslSocket.h"
#include "galay-http/kernel/SslRecvCompatAwaitable.h"
#endif

namespace galay::http2
{

using namespace galay::kernel;

// 前向声明 StreamManager
template<typename SocketType>
class Http2StreamManagerImpl;

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
    
    Http2ErrorCode applySettings(const Http2SettingsFrame& frame) {
        for (const auto& setting : frame.settings()) {
            switch (setting.id) {
                case Http2SettingsId::HeaderTableSize:
                    header_table_size = setting.value;
                    break;
                case Http2SettingsId::EnablePush:
                    if (setting.value > 1) return Http2ErrorCode::ProtocolError;
                    enable_push = setting.value;
                    break;
                case Http2SettingsId::MaxConcurrentStreams:
                    max_concurrent_streams = setting.value;
                    break;
                case Http2SettingsId::InitialWindowSize:
                    if (setting.value > 2147483647u) return Http2ErrorCode::FlowControlError;
                    initial_window_size = setting.value;
                    break;
                case Http2SettingsId::MaxFrameSize:
                    if (setting.value < 16384 || setting.value > 16777215) return Http2ErrorCode::ProtocolError;
                    max_frame_size = setting.value;
                    break;
                case Http2SettingsId::MaxHeaderListSize:
                    max_header_list_size = setting.value;
                    break;
            }
        }
        return Http2ErrorCode::NoError;
    }
    
    template<typename Config>
    void from(const Config& config) {
        if constexpr (requires { config.header_table_size; })
            header_table_size = config.header_table_size;
        if constexpr (requires { config.enable_push; }) {
            if constexpr (std::is_same_v<decltype(config.enable_push), const bool>)
                enable_push = config.enable_push ? 1 : 0;
            else
                enable_push = config.enable_push;
        }
        if constexpr (requires { config.max_concurrent_streams; })
            max_concurrent_streams = config.max_concurrent_streams;
        if constexpr (requires { config.initial_window_size; })
            initial_window_size = config.initial_window_size;
        if constexpr (requires { config.max_frame_size; })
            max_frame_size = config.max_frame_size;
        if constexpr (requires { config.max_header_list_size; })
            max_header_list_size = config.max_header_list_size;
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

struct Http2FlowControlUpdate
{
    uint32_t conn_increment = 0;
    uint32_t stream_increment = 0;
};

using Http2FlowControlStrategy = std::function<Http2FlowControlUpdate(
    int32_t conn_recv_window,
    int32_t stream_recv_window,
    uint32_t target_window,
    size_t data_size)>;

struct Http2RuntimeConfig
{
    bool ping_enabled = true;
    std::chrono::milliseconds ping_interval{30000};
    std::chrono::milliseconds ping_timeout{10000};
    std::chrono::milliseconds settings_ack_timeout{10000};
    std::chrono::milliseconds graceful_shutdown_rtt{100};
    std::chrono::milliseconds graceful_shutdown_timeout{5000};
    uint32_t flow_control_target_window = kDefaultInitialWindowSize;
    Http2FlowControlStrategy flow_control_strategy;

    template<typename Config>
    void from(const Config& config) {
        if constexpr (requires { config.ping_enabled; }) {
            ping_enabled = config.ping_enabled;
        }
        if constexpr (requires { config.ping_interval; }) {
            ping_interval = config.ping_interval;
        }
        if constexpr (requires { config.ping_timeout; }) {
            ping_timeout = config.ping_timeout;
        }
        if constexpr (requires { config.settings_ack_timeout; }) {
            settings_ack_timeout = config.settings_ack_timeout;
        }
        if constexpr (requires { config.graceful_shutdown_rtt; }) {
            graceful_shutdown_rtt = config.graceful_shutdown_rtt;
        }
        if constexpr (requires { config.graceful_shutdown_timeout; }) {
            graceful_shutdown_timeout = config.graceful_shutdown_timeout;
        }
        if constexpr (requires { config.flow_control_target_window; }) {
            flow_control_target_window = config.flow_control_target_window;
        }
        if constexpr (requires { config.flow_control_strategy; }) {
            flow_control_strategy = config.flow_control_strategy;
        }
    }
};

// 前向声明
template<typename SocketType>
class Http2ConnImpl;

/**
 * @brief HTTP/2 帧读取等待体 - TcpSocket 版本
 */
template<typename SocketType, bool IsSsl = is_ssl_socket_v<SocketType>>
class Http2ReadFrameAwaitableImpl;

// TcpSocket 特化版本（CustomAwaitable + RecvAwaitable）
template<typename SocketType>
class Http2ReadFrameAwaitableImpl<SocketType, false>
    : public CustomAwaitable
    , public TimeoutSupport<Http2ReadFrameAwaitableImpl<SocketType, false>>
{
public:
    class ProtocolRecvAwaitable : public RecvAwaitable
    {
    public:
        explicit ProtocolRecvAwaitable(Http2ReadFrameAwaitableImpl* owner)
            : RecvAwaitable(owner->m_socket.controller(), nullptr, 0)
            , m_owner(owner)
        {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle) override {
            if (m_owner->m_closing && *m_owner->m_closing) {
                m_owner->setProtocolError(Http2ErrorCode::ProtocolError, "Connection closing");
                return true;
            }

            if (m_owner->checkBufferedFrame()) {
                return true;
            }

            if (cqe == nullptr) {
                if (!prepareRecvWindow()) {
                    m_owner->setProtocolError(Http2ErrorCode::ProtocolError, "RingBuffer is full");
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

            size_t bytes_read = result.value().size();
            if (bytes_read == 0) {
                m_owner->setProtocolError(Http2ErrorCode::ProtocolError, "peer closed");
                return true;
            }

            m_owner->m_ring_buffer.produce(bytes_read);
            if (m_owner->m_last_error_msg) {
                m_owner->m_last_error_msg->clear();
            }

            if (m_owner->checkBufferedFrame()) {
                return true;
            }

            if (!prepareRecvWindow()) {
                m_owner->setProtocolError(Http2ErrorCode::ProtocolError, "RingBuffer is full");
                return true;
            }
            return false;
        }
#else
        bool handleComplete(GHandle handle) override {
            if (m_owner->m_closing && *m_owner->m_closing) {
                m_owner->setProtocolError(Http2ErrorCode::ProtocolError, "Connection closing");
                return true;
            }

            if (m_owner->checkBufferedFrame()) {
                return true;
            }

            while (true) {
                if (!prepareRecvWindow()) {
                    m_owner->setProtocolError(Http2ErrorCode::ProtocolError, "RingBuffer is full");
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

                size_t bytes_read = result.value().size();
                if (bytes_read == 0) {
                    m_owner->setProtocolError(Http2ErrorCode::ProtocolError, "peer closed");
                    return true;
                }

                m_owner->m_ring_buffer.produce(bytes_read);
                if (m_owner->m_last_error_msg) {
                    m_owner->m_last_error_msg->clear();
                }

                if (m_owner->checkBufferedFrame()) {
                    return true;
                }
            }
        }
#endif

    private:
        bool prepareRecvWindow() {
            auto write_iovecs = m_owner->m_ring_buffer.getWriteIovecs();
            if (write_iovecs.empty()) {
                return false;
            }
            m_buffer = static_cast<char*>(write_iovecs[0].iov_base);
            m_length = write_iovecs[0].iov_len;
            return m_length > 0;
        }

        Http2ReadFrameAwaitableImpl* m_owner;
    };

    Http2ReadFrameAwaitableImpl(RingBuffer& ring_buffer,
                                Http2Settings& peer_settings,
                                SocketType& socket,
                                bool* peer_closed = nullptr,
                                std::string* last_error_msg = nullptr,
                                const bool* closing = nullptr)
        : CustomAwaitable(socket.controller())
        , m_ring_buffer(ring_buffer)
        , m_peer_settings(peer_settings)
        , m_socket(socket)
        , m_peer_closed(peer_closed)
        , m_last_error_msg(last_error_msg)
        , m_closing(closing)
        , m_has_buffered_frame(false)
        , m_buffered_total_frame_size(0)
        , m_has_async_task(false)
        , m_recv_awaitable(this)
        , m_result(true)
    {
        // 检查 RingBuffer 中是否已经有完整的帧
        checkBufferedFrame();
        if (!(m_closing && *m_closing) && !m_has_buffered_frame) {
            addTask(IOEventType::RECV, &m_recv_awaitable);
            m_has_async_task = true;
        }
    }

    bool await_ready() const noexcept {
        // 连接已关闭时不挂起，直接返回错误
        if (m_closing && *m_closing) return true;
        return m_has_buffered_frame;
    }

    using CustomAwaitable::await_suspend;

    std::expected<Http2Frame::uptr, Http2ErrorCode> await_resume() {
        // 连接已关闭
        if (m_closing && *m_closing && !m_has_buffered_frame) {
            if (m_last_error_msg) {
                *m_last_error_msg = "Connection closing";
            }
            return std::unexpected(Http2ErrorCode::ProtocolError);
        }

        if (m_has_async_task) {
            onCompleted();

            if (!m_result.has_value()) {
                setRecvError(m_result.error());
            }
        }

        if (m_error.has_value()) {
            return std::unexpected(*m_error);
        }

        HTTP_LOG_DEBUG("[readFrame] [resume] [buffered]");
        return parseFrameFromBuffer();
    }

private:
    bool checkBufferedFrame() {
        m_buffered_total_frame_size = 0;
        if (m_ring_buffer.readable() < kHttp2FrameHeaderLength) {
            m_has_buffered_frame = false;
            return false;
        }

        auto read_iovecs = m_ring_buffer.getReadIovecs();
        if (read_iovecs.empty()) {
            m_has_buffered_frame = false;
            return false;
        }

        Http2FrameHeader header;
        if (read_iovecs[0].iov_len >= kHttp2FrameHeaderLength) {
            header = Http2FrameHeader::deserialize(
                static_cast<const uint8_t*>(read_iovecs[0].iov_base));
        } else {
            uint8_t header_buf[kHttp2FrameHeaderLength];
            size_t copied = 0;
            for (const auto& iov : read_iovecs) {
                size_t to_copy = std::min(iov.iov_len, kHttp2FrameHeaderLength - copied);
                std::memcpy(header_buf + copied, iov.iov_base, to_copy);
                copied += to_copy;
                if (copied >= kHttp2FrameHeaderLength) break;
            }
            header = Http2FrameHeader::deserialize(header_buf);
        }
        if (header.length > m_peer_settings.max_frame_size) {
            setProtocolError(Http2ErrorCode::FrameSizeError, "frame too large");
            m_has_buffered_frame = true;
            return true;
        }
        size_t total_frame_size = kHttp2FrameHeaderLength + header.length;

        m_has_buffered_frame = (m_ring_buffer.readable() >= total_frame_size);
        if (m_has_buffered_frame) {
            m_buffered_total_frame_size = total_frame_size;
        }
        return m_has_buffered_frame;
    }

    void setRecvError(const IOError& io_error) {
        if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
            if (m_peer_closed) {
                *m_peer_closed = true;
            }
        }
        if (m_last_error_msg) {
            *m_last_error_msg = io_error.message();
        }
        HTTP_LOG_DEBUG("[readFrame] [recv-fail] [{}]", io_error.message());
        m_error = Http2ErrorCode::ProtocolError;
    }

    void setProtocolError(Http2ErrorCode code, const std::string& msg) {
        if (code == Http2ErrorCode::ProtocolError && msg == "peer closed" && m_peer_closed) {
            *m_peer_closed = true;
        }
        if (m_last_error_msg) {
            *m_last_error_msg = msg;
        }
        m_error = code;
    }

    std::expected<Http2Frame::uptr, Http2ErrorCode> parseFrameFromBuffer() {
        if (m_ring_buffer.readable() < kHttp2FrameHeaderLength) {
            return std::unexpected(Http2ErrorCode::NoError);
        }

        auto read_iovecs = m_ring_buffer.getReadIovecs();
        if (read_iovecs.empty()) {
            return std::unexpected(Http2ErrorCode::NoError);
        }

        size_t total_frame_size = m_buffered_total_frame_size;
        if (total_frame_size == 0 || m_ring_buffer.readable() < total_frame_size) {
            Http2FrameHeader header;
            if (read_iovecs[0].iov_len >= kHttp2FrameHeaderLength) {
                header = Http2FrameHeader::deserialize(
                    static_cast<const uint8_t*>(read_iovecs[0].iov_base));
            } else {
                uint8_t header_buf[kHttp2FrameHeaderLength];
                size_t copied = 0;
                for (const auto& iov : read_iovecs) {
                    size_t to_copy = std::min(iov.iov_len, kHttp2FrameHeaderLength - copied);
                    std::memcpy(header_buf + copied, iov.iov_base, to_copy);
                    copied += to_copy;
                    if (copied >= kHttp2FrameHeaderLength) break;
                }
                header = Http2FrameHeader::deserialize(header_buf);
            }

            if (header.length > m_peer_settings.max_frame_size) {
                return std::unexpected(Http2ErrorCode::FrameSizeError);
            }

            total_frame_size = kHttp2FrameHeaderLength + header.length;
        }

        if (m_ring_buffer.readable() < total_frame_size) {
            return std::unexpected(Http2ErrorCode::NoError);
        }

        std::expected<Http2Frame::uptr, Http2ErrorCode> frame_result =
            std::unexpected(Http2ErrorCode::ProtocolError);

        if (read_iovecs[0].iov_len >= total_frame_size) {
            frame_result = Http2FrameParser::parseFrame(
                static_cast<const uint8_t*>(read_iovecs[0].iov_base), total_frame_size);
        } else {
            if (m_parse_buffer.size() < total_frame_size) {
                m_parse_buffer.resize(total_frame_size);
            }
            size_t copied = 0;
            for (const auto& iov : read_iovecs) {
                size_t to_copy = std::min(iov.iov_len, total_frame_size - copied);
                std::memcpy(m_parse_buffer.data() + copied, iov.iov_base, to_copy);
                copied += to_copy;
                if (copied >= total_frame_size) break;
            }
            frame_result = Http2FrameParser::parseFrame(m_parse_buffer.data(), total_frame_size);
        }

        m_buffered_total_frame_size = 0;
        if (frame_result) {
            m_ring_buffer.consume(total_frame_size);
        }

        return frame_result;
    }

    RingBuffer& m_ring_buffer;
    Http2Settings& m_peer_settings;
    SocketType& m_socket;
    bool* m_peer_closed;
    std::string* m_last_error_msg;
    const bool* m_closing;
    bool m_has_buffered_frame;
    size_t m_buffered_total_frame_size;
    std::vector<uint8_t> m_parse_buffer;
    bool m_has_async_task;
    ProtocolRecvAwaitable m_recv_awaitable;
    std::optional<Http2ErrorCode> m_error;

public:
    std::expected<bool, IOError> m_result;
};

#ifdef GALAY_HTTP_SSL_ENABLED
// SslSocket 特化版本（CustomAwaitable + SslRecvAwaitable）
template<typename SocketType>
class Http2ReadFrameAwaitableImpl<SocketType, true>
    : public CustomAwaitable
    , public TimeoutSupport<Http2ReadFrameAwaitableImpl<SocketType, true>>
{
public:
    using RecvAwaitableType = galay::http::SslRecvCompatAwaitable;

    class ProtocolRecvAwaitable : public RecvAwaitableType
    {
    public:
        explicit ProtocolRecvAwaitable(Http2ReadFrameAwaitableImpl* owner)
            : RecvAwaitableType(owner->m_socket.recv(owner->m_dummy_recv_buffer, sizeof(owner->m_dummy_recv_buffer)))
            , m_owner(owner)
        {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override {
            if (m_owner->m_closing && *m_owner->m_closing) {
                m_owner->setProtocolError(Http2ErrorCode::ProtocolError, "Connection closing");
                return true;
            }

            if (m_owner->checkBufferedFrame()) {
                return true;
            }

            if (!prepareRecvWindow()) {
                m_owner->setProtocolError(Http2ErrorCode::ProtocolError, "RingBuffer is full");
                return true;
            }

            if (cqe == nullptr) {
                return false;
            }

            this->m_sslResultSet = false;
            const bool done = RecvAwaitableType::handleComplete(cqe, handle);
            if (!done) {
                return false;
            }

            auto recv_result = std::move(this->m_sslResult);
            this->m_sslResultSet = false;

            if (!recv_result) {
                const auto& error = recv_result.error();
                if (error.sslError() == SSL_ERROR_WANT_READ || error.sslError() == SSL_ERROR_WANT_WRITE) {
                    return false;
                }
                m_owner->setSslRecvError(error);
                return true;
            }

            const size_t bytes_read = recv_result.value().size();
            if (bytes_read == 0) {
                m_owner->setProtocolError(Http2ErrorCode::ProtocolError, "peer closed");
                return true;
            }

            m_owner->m_ring_buffer.produce(bytes_read);
            if (m_owner->m_last_error_msg) {
                m_owner->m_last_error_msg->clear();
            }

            if (m_owner->checkBufferedFrame()) {
                return true;
            }

            if (!prepareRecvWindow()) {
                m_owner->setProtocolError(Http2ErrorCode::ProtocolError, "RingBuffer is full");
                return true;
            }
            return false;
        }
#else
        bool handleComplete(GHandle handle) override {
            if (m_owner->m_closing && *m_owner->m_closing) {
                m_owner->setProtocolError(Http2ErrorCode::ProtocolError, "Connection closing");
                return true;
            }

            if (m_owner->checkBufferedFrame()) {
                return true;
            }

            while (true) {
                if (!prepareRecvWindow()) {
                    m_owner->setProtocolError(Http2ErrorCode::ProtocolError, "RingBuffer is full");
                    return true;
                }

                this->m_sslResultSet = false;
                const bool done = RecvAwaitableType::handleComplete(handle);
                if (!done) {
                    return false;
                }

                auto recv_result = std::move(this->m_sslResult);
                this->m_sslResultSet = false;

                if (!recv_result) {
                    const auto& error = recv_result.error();
                    if (error.sslError() == SSL_ERROR_WANT_READ || error.sslError() == SSL_ERROR_WANT_WRITE) {
                        return false;
                    }
                    m_owner->setSslRecvError(error);
                    return true;
                }

                const size_t bytes_read = recv_result.value().size();
                if (bytes_read == 0) {
                    m_owner->setProtocolError(Http2ErrorCode::ProtocolError, "peer closed");
                    return true;
                }

                m_owner->m_ring_buffer.produce(bytes_read);
                if (m_owner->m_last_error_msg) {
                    m_owner->m_last_error_msg->clear();
                }

                if (m_owner->checkBufferedFrame()) {
                    return true;
                }
            }
        }
#endif

    private:
        bool prepareRecvWindow() {
            auto write_iovecs = m_owner->m_ring_buffer.getWriteIovecs();
            if (write_iovecs.empty()) {
                return false;
            }
            this->m_plainBuffer = static_cast<char*>(write_iovecs[0].iov_base);
            this->m_plainLength = write_iovecs[0].iov_len;
            return this->m_plainLength > 0;
        }

        Http2ReadFrameAwaitableImpl* m_owner;
    };

    Http2ReadFrameAwaitableImpl(RingBuffer& ring_buffer,
                                Http2Settings& peer_settings,
                                SocketType& socket,
                                bool* peer_closed = nullptr,
                                std::string* last_error_msg = nullptr,
                                const bool* closing = nullptr)
        : CustomAwaitable(socket.controller())
        , m_ring_buffer(ring_buffer)
        , m_peer_settings(peer_settings)
        , m_socket(socket)
        , m_peer_closed(peer_closed)
        , m_last_error_msg(last_error_msg)
        , m_closing(closing)
        , m_has_buffered_frame(false)
        , m_buffered_total_frame_size(0)
        , m_has_async_task(false)
        , m_recv_awaitable(this)
        , m_result(true)
    {
        checkBufferedFrame();
        if (!(m_closing && *m_closing) && !m_has_buffered_frame) {
            addTask(IOEventType::RECV, &m_recv_awaitable);
            m_has_async_task = true;
        }
    }

    bool await_ready() const noexcept {
        if (m_closing && *m_closing) return true;
        return m_has_buffered_frame;
    }

    using CustomAwaitable::await_suspend;

    std::expected<Http2Frame::uptr, Http2ErrorCode> await_resume() {
        if (m_closing && *m_closing && !m_has_buffered_frame) {
            if (m_last_error_msg) {
                *m_last_error_msg = "Connection closing";
            }
            return std::unexpected(Http2ErrorCode::ProtocolError);
        }

        if (m_has_async_task) {
            onCompleted();

            if (!m_result.has_value()) {
                setRecvError(m_result.error());
            }
        }

        if (m_error.has_value()) {
            return std::unexpected(*m_error);
        }

        return parseFrameFromBuffer();
    }

private:
    bool checkBufferedFrame() {
        m_buffered_total_frame_size = 0;
        if (m_ring_buffer.readable() < kHttp2FrameHeaderLength) {
            m_has_buffered_frame = false;
            return false;
        }

        auto read_iovecs = m_ring_buffer.getReadIovecs();
        if (read_iovecs.empty()) {
            m_has_buffered_frame = false;
            return false;
        }

        Http2FrameHeader header;
        if (read_iovecs[0].iov_len >= kHttp2FrameHeaderLength) {
            header = Http2FrameHeader::deserialize(
                static_cast<const uint8_t*>(read_iovecs[0].iov_base));
        } else {
            uint8_t header_buf[kHttp2FrameHeaderLength];
            size_t copied = 0;
            for (const auto& iov : read_iovecs) {
                size_t to_copy = std::min(iov.iov_len, kHttp2FrameHeaderLength - copied);
                std::memcpy(header_buf + copied, iov.iov_base, to_copy);
                copied += to_copy;
                if (copied >= kHttp2FrameHeaderLength) break;
            }
            header = Http2FrameHeader::deserialize(header_buf);
        }
        if (header.length > m_peer_settings.max_frame_size) {
            setProtocolError(Http2ErrorCode::FrameSizeError, "frame too large");
            m_has_buffered_frame = true;
            return true;
        }
        size_t total_frame_size = kHttp2FrameHeaderLength + header.length;

        m_has_buffered_frame = (m_ring_buffer.readable() >= total_frame_size);
        if (m_has_buffered_frame) {
            m_buffered_total_frame_size = total_frame_size;
        }
        return m_has_buffered_frame;
    }

    void setRecvError(const IOError& io_error) {
        if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
            if (m_peer_closed) {
                *m_peer_closed = true;
            }
        }
        if (m_last_error_msg) {
            *m_last_error_msg = io_error.message();
        }
        m_error = Http2ErrorCode::ProtocolError;
    }

    void setSslRecvError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            setProtocolError(Http2ErrorCode::ProtocolError, "peer closed");
            return;
        }
        setProtocolError(Http2ErrorCode::ProtocolError, error.message());
    }

    void setProtocolError(Http2ErrorCode code, const std::string& msg) {
        if (code == Http2ErrorCode::ProtocolError && msg == "peer closed" && m_peer_closed) {
            *m_peer_closed = true;
        }
        if (m_last_error_msg) {
            *m_last_error_msg = msg;
        }
        m_error = code;
    }

    std::expected<Http2Frame::uptr, Http2ErrorCode> parseFrameFromBuffer() {
        if (m_ring_buffer.readable() < kHttp2FrameHeaderLength) {
            return std::unexpected(Http2ErrorCode::NoError);
        }

        auto read_iovecs = m_ring_buffer.getReadIovecs();
        if (read_iovecs.empty()) {
            return std::unexpected(Http2ErrorCode::NoError);
        }

        size_t total_frame_size = m_buffered_total_frame_size;
        if (total_frame_size == 0 || m_ring_buffer.readable() < total_frame_size) {
            Http2FrameHeader header;
            if (read_iovecs[0].iov_len >= kHttp2FrameHeaderLength) {
                header = Http2FrameHeader::deserialize(
                    static_cast<const uint8_t*>(read_iovecs[0].iov_base));
            } else {
                uint8_t header_buf[kHttp2FrameHeaderLength];
                size_t copied = 0;
                for (const auto& iov : read_iovecs) {
                    size_t to_copy = std::min(iov.iov_len, kHttp2FrameHeaderLength - copied);
                    std::memcpy(header_buf + copied, iov.iov_base, to_copy);
                    copied += to_copy;
                    if (copied >= kHttp2FrameHeaderLength) break;
                }
                header = Http2FrameHeader::deserialize(header_buf);
            }

            if (header.length > m_peer_settings.max_frame_size) {
                return std::unexpected(Http2ErrorCode::FrameSizeError);
            }

            total_frame_size = kHttp2FrameHeaderLength + header.length;
        }
        if (m_ring_buffer.readable() < total_frame_size) {
            return std::unexpected(Http2ErrorCode::NoError);
        }

        std::expected<Http2Frame::uptr, Http2ErrorCode> frame_result =
            std::unexpected(Http2ErrorCode::ProtocolError);
        if (read_iovecs[0].iov_len >= total_frame_size) {
            frame_result = Http2FrameParser::parseFrame(
                static_cast<const uint8_t*>(read_iovecs[0].iov_base), total_frame_size);
        } else {
            if (m_parse_buffer.size() < total_frame_size) {
                m_parse_buffer.resize(total_frame_size);
            }
            size_t copied = 0;
            for (const auto& iov : read_iovecs) {
                size_t to_copy = std::min(iov.iov_len, total_frame_size - copied);
                std::memcpy(m_parse_buffer.data() + copied, iov.iov_base, to_copy);
                copied += to_copy;
                if (copied >= total_frame_size) break;
            }
            frame_result = Http2FrameParser::parseFrame(m_parse_buffer.data(), total_frame_size);
        }
        m_buffered_total_frame_size = 0;
        if (frame_result) {
            m_ring_buffer.consume(total_frame_size);
        }
        return frame_result;
    }

    RingBuffer& m_ring_buffer;
    Http2Settings& m_peer_settings;
    SocketType& m_socket;
    bool* m_peer_closed;
    std::string* m_last_error_msg;
    const bool* m_closing;
    bool m_has_buffered_frame;
    size_t m_buffered_total_frame_size;
    std::vector<uint8_t> m_parse_buffer;
    bool m_has_async_task;
    char m_dummy_recv_buffer[1];
    ProtocolRecvAwaitable m_recv_awaitable;
    std::optional<Http2ErrorCode> m_error;

public:
    std::expected<bool, IOError> m_result;
};
#endif

/**
 * @brief HTTP/2 帧写入等待体 - TcpSocket 版本
 */
template<typename SocketType, bool IsSsl = is_ssl_socket_v<SocketType>>
class Http2WriteFrameAwaitableImpl;

// TcpSocket 特化版本
template<typename SocketType>
class Http2WriteFrameAwaitableImpl<SocketType, false>
    : public CustomAwaitable
    , public TimeoutSupport<Http2WriteFrameAwaitableImpl<SocketType, false>>
{
public:
    class ProtocolSendAwaitable : public SendAwaitable
    {
    public:
        explicit ProtocolSendAwaitable(Http2WriteFrameAwaitableImpl* owner)
            : SendAwaitable(owner->m_socket.controller(),
                            owner->m_data.data(),
                            owner->m_data.size())
            , m_owner(owner)
        {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle) override {
            if (m_owner->m_offset >= m_owner->m_data.size()) {
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

            m_owner->m_offset += sent;
            HTTP_LOG_DEBUG("[writeFrame] [resume] [sent={}] [progress={}/{}]",
                          sent, m_owner->m_offset, m_owner->m_data.size());
            if (m_owner->m_offset >= m_owner->m_data.size()) {
                return true;
            }

            syncSendWindow();
            return false;
        }
#else
        bool handleComplete(GHandle handle) override {
            while (m_owner->m_offset < m_owner->m_data.size()) {
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

                m_owner->m_offset += sent;
                HTTP_LOG_DEBUG("[writeFrame] [resume] [sent={}] [progress={}/{}]",
                              sent, m_owner->m_offset, m_owner->m_data.size());
            }

            return true;
        }
#endif

    private:
        void syncSendWindow() {
            if (m_owner->m_offset >= m_owner->m_data.size()) {
                m_buffer = nullptr;
                m_length = 0;
                return;
            }
            m_buffer = m_owner->m_data.data() + m_owner->m_offset;
            m_length = m_owner->m_data.size() - m_owner->m_offset;
        }

        Http2WriteFrameAwaitableImpl* m_owner;
    };

    Http2WriteFrameAwaitableImpl(SocketType& socket, std::string data)
        : CustomAwaitable(socket.controller())
        , m_socket(socket)
        , m_data(std::move(data))
        , m_offset(0)
        , m_send_awaitable(this)
        , m_result(true)
    {
        if (!m_data.empty()) {
            addTask(IOEventType::SEND, &m_send_awaitable);
        }
    }
    
    bool await_ready() const noexcept { return m_data.empty(); }
    
    using CustomAwaitable::await_suspend;

    std::expected<bool, Http2ErrorCode> await_resume() {
        if (m_data.empty()) {
            return true;
        }

        onCompleted();

        if (!m_result.has_value()) {
            HTTP_LOG_ERROR("[writeFrame] [send-fail] [{}]", m_result.error().message());
            return std::unexpected(Http2ErrorCode::InternalError);
        }
        if (m_error.has_value()) {
            return std::unexpected(*m_error);
        }

        return true;
    }

private:
    void setSendError(const IOError& io_error) {
        HTTP_LOG_ERROR("[writeFrame] [send-fail] [{}]", io_error.message());
        m_error = Http2ErrorCode::InternalError;
    }

    SocketType& m_socket;
    std::string m_data;
    size_t m_offset;
    ProtocolSendAwaitable m_send_awaitable;
    std::optional<Http2ErrorCode> m_error;

public:
    std::expected<bool, IOError> m_result;
};

#ifdef GALAY_HTTP_SSL_ENABLED
// SslSocket 特化版本（CustomAwaitable + SslSendAwaitable）
template<typename SocketType>
class Http2WriteFrameAwaitableImpl<SocketType, true>
    : public CustomAwaitable
    , public TimeoutSupport<Http2WriteFrameAwaitableImpl<SocketType, true>>
{
public:
    using SendAwaitableType = decltype(std::declval<SocketType>().send(std::declval<const char*>(), std::declval<size_t>()));

    class ProtocolSendAwaitable : public SendAwaitableType
    {
    public:
        explicit ProtocolSendAwaitable(Http2WriteFrameAwaitableImpl* owner)
            : SendAwaitableType(owner->m_socket.send(owner->m_data.data(), owner->m_data.size()))
            , m_owner(owner)
        {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override {
            if (m_owner->m_data.empty()) {
                return true;
            }

            if (cqe == nullptr) {
                return false;
            }

            this->m_sslResultSet = false;
            const bool done = SendAwaitableType::handleComplete(cqe, handle);
            if (!done) {
                return false;
            }

            auto send_result = std::move(this->m_sslResult);
            this->m_sslResultSet = false;

            if (!send_result) {
                const auto& error = send_result.error();
                if (error.sslError() == SSL_ERROR_WANT_READ || error.sslError() == SSL_ERROR_WANT_WRITE) {
                    return false;
                }
                m_owner->setSslSendError(error);
                return true;
            }

            const size_t sent = send_result.value();
            if (sent == 0) {
                return false;
            }
            if (sent < m_owner->m_data.size()) {
                m_owner->setProtocolError("partial ssl send");
                return true;
            }
            return true;
        }
#else
        bool handleComplete(GHandle handle) override {
            if (m_owner->m_data.empty()) {
                return true;
            }

            this->m_sslResultSet = false;
            const bool done = SendAwaitableType::handleComplete(handle);
            if (!done) {
                return false;
            }

            auto send_result = std::move(this->m_sslResult);
            this->m_sslResultSet = false;

            if (!send_result) {
                const auto& error = send_result.error();
                if (error.sslError() == SSL_ERROR_WANT_READ || error.sslError() == SSL_ERROR_WANT_WRITE) {
                    return false;
                }
                m_owner->setSslSendError(error);
                return true;
            }

            const size_t sent = send_result.value();
            if (sent == 0) {
                return false;
            }
            if (sent < m_owner->m_data.size()) {
                m_owner->setProtocolError("partial ssl send");
                return true;
            }
            return true;
        }
#endif

        Http2WriteFrameAwaitableImpl* m_owner;
    };

    Http2WriteFrameAwaitableImpl(SocketType& socket, std::string data)
        : CustomAwaitable(socket.controller())
        , m_socket(socket)
        , m_data(std::move(data))
        , m_has_async_task(false)
        , m_send_awaitable(this)
        , m_result(true)
    {
        if (!m_data.empty()) {
            addTask(IOEventType::SEND, &m_send_awaitable);
            m_has_async_task = true;
        }
    }

    bool await_ready() const noexcept { return m_data.empty(); }

    using CustomAwaitable::await_suspend;

    std::expected<bool, Http2ErrorCode> await_resume() {
        if (m_data.empty()) {
            return true;
        }

        if (m_has_async_task) {
            onCompleted();
            if (!m_result.has_value()) {
                setSendError(m_result.error());
            }
        }

        if (m_error.has_value()) {
            return std::unexpected(*m_error);
        }

        return true;
    }

private:
    void setSendError(const IOError& io_error) {
        HTTP_LOG_ERROR("[writeFrame] [send-fail] [{}]", io_error.message());
        m_error = Http2ErrorCode::InternalError;
    }

    void setSslSendError(const galay::ssl::SslError& error) {
        HTTP_LOG_ERROR("[writeFrame] [ssl-send-fail] [{}]", error.message());
        m_error = Http2ErrorCode::InternalError;
    }

    void setProtocolError(const std::string&) {
        m_error = Http2ErrorCode::InternalError;
    }

    SocketType& m_socket;
    std::string m_data;
    bool m_has_async_task;
    ProtocolSendAwaitable m_send_awaitable;
    std::optional<Http2ErrorCode> m_error;

public:
    std::expected<bool, IOError> m_result;
};
#endif

/**
 * @brief HTTP/2 批量帧读取等待体 - TcpSocket 版本
 */
template<typename SocketType, bool IsSsl = is_ssl_socket_v<SocketType>>
class Http2ReadFramesBatchAwaitableImpl;

// TcpSocket 特化版本（CustomAwaitable + RecvAwaitable）
template<typename SocketType>
class Http2ReadFramesBatchAwaitableImpl<SocketType, false>
    : public CustomAwaitable
    , public TimeoutSupport<Http2ReadFramesBatchAwaitableImpl<SocketType, false>>
{
public:
    class ProtocolReadvAwaitable : public ReadvIOContext
    {
    public:
        explicit ProtocolReadvAwaitable(Http2ReadFramesBatchAwaitableImpl* owner)
            : ReadvIOContext({})
            , m_owner(owner)
        {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override {
            if (m_owner->m_closing && *m_owner->m_closing) {
                m_owner->setProtocolError(Http2ErrorCode::ProtocolError, "Connection closing");
                return true;
            }

            if (m_owner->checkBufferedFrames()) {
                return true;
            }

            if (cqe == nullptr) {
                if (!prepareReadvWindow()) {
                    m_owner->setProtocolError(Http2ErrorCode::ProtocolError, "RingBuffer is full");
                    return true;
                }
                return false;
            }

            // 使用 ReadvIOContext 的 handleComplete 处理 readv 结果
            if (!ReadvIOContext::handleComplete(cqe, handle)) {
                return false;
            }

            if (!m_result) {
                m_owner->setRecvError(m_result.error());
                return true;
            }

            size_t bytes_read = m_result.value();
            if (bytes_read == 0) {
                m_owner->setProtocolError(Http2ErrorCode::ProtocolError, "peer closed");
                return true;
            }

            m_owner->m_ring_buffer.produce(bytes_read);
            if (m_owner->m_last_error_msg) {
                m_owner->m_last_error_msg->clear();
            }

            if (m_owner->checkBufferedFrames()) {
                return true;
            }

            if (!prepareReadvWindow()) {
                m_owner->setProtocolError(Http2ErrorCode::ProtocolError, "RingBuffer is full");
                return true;
            }
            return false;
        }
#else
        bool handleComplete(GHandle handle) override {
            if (m_owner->m_closing && *m_owner->m_closing) {
                m_owner->setProtocolError(Http2ErrorCode::ProtocolError, "Connection closing");
                return true;
            }

            if (m_owner->checkBufferedFrames()) {
                return true;
            }

            while (true) {
                if (!prepareReadvWindow()) {
                    m_owner->setProtocolError(Http2ErrorCode::ProtocolError, "RingBuffer is full");
                    return true;
                }

                // 使用 ReadvIOContext 的 handleComplete 处理 readv 结果
                if (!ReadvIOContext::handleComplete(handle)) {
                    return false;
                }

                if (!m_result) {
                    m_owner->setRecvError(m_result.error());
                    return true;
                }

                size_t bytes_read = m_result.value();
                if (bytes_read == 0) {
                    m_owner->setProtocolError(Http2ErrorCode::ProtocolError, "peer closed");
                    return true;
                }

                m_owner->m_ring_buffer.produce(bytes_read);
                if (m_owner->m_last_error_msg) {
                    m_owner->m_last_error_msg->clear();
                }

                if (m_owner->checkBufferedFrames()) {
                    return true;
                }
            }
        }
#endif

    private:
        bool prepareReadvWindow() {
            struct iovec write_iovecs[2];
            const size_t iov_count = m_owner->m_ring_buffer.getWriteIovecs(write_iovecs, 2);
            if (iov_count == 0) {
                return false;
            }
            m_iovecs.assign(write_iovecs, write_iovecs + iov_count);
            return true;
        }

        Http2ReadFramesBatchAwaitableImpl* m_owner;
    };

    Http2ReadFramesBatchAwaitableImpl(RingBuffer& ring_buffer,
                                      Http2Settings& peer_settings,
                                      SocketType& socket,
                                      size_t max_frames,
                                      bool* peer_closed = nullptr,
                                      std::string* last_error_msg = nullptr,
                                      const bool* closing = nullptr)
        : CustomAwaitable(socket.controller())
        , m_ring_buffer(ring_buffer)
        , m_peer_settings(peer_settings)
        , m_socket(socket)
        , m_max_frames(max_frames)
        , m_peer_closed(peer_closed)
        , m_last_error_msg(last_error_msg)
        , m_closing(closing)
        , m_has_buffered_frames(false)
        , m_has_async_task(false)
        , m_recv_awaitable(this)
        , m_result(true)
    {
        // 检查 RingBuffer 中是否已经有完整的帧
        checkBufferedFrames();
        if (!(m_closing && *m_closing) && !m_has_buffered_frames) {
            addTask(IOEventType::RECV, &m_recv_awaitable);
            m_has_async_task = true;
        }
    }

    bool await_ready() const noexcept {
        // 连接已关闭时不挂起，直接返回错误
        if (m_closing && *m_closing) return true;
        return m_has_buffered_frames;
    }

    using CustomAwaitable::await_suspend;

    std::expected<std::vector<Http2Frame::uptr>, Http2ErrorCode> await_resume() {
        // 连接已关闭
        if (m_closing && *m_closing && !m_has_buffered_frames) {
            if (m_last_error_msg) {
                *m_last_error_msg = "Connection closing";
            }
            return std::unexpected(Http2ErrorCode::ProtocolError);
        }

        if (m_has_async_task) {
            onCompleted();

            if (!m_result.has_value()) {
                setRecvError(m_result.error());
            }
        }

        if (m_error.has_value()) {
            return std::unexpected(*m_error);
        }

        HTTP_LOG_DEBUG("[readFramesBatch] [resume] [buffered]");
        return parseFramesFromBuffer();
    }

private:
    bool checkBufferedFrames() {
        // 尝试解析至少一个完整帧
        if (m_ring_buffer.readable() < kHttp2FrameHeaderLength) {
            m_has_buffered_frames = false;
            return false;
        }

        struct iovec read_iovecs[2];
        const size_t iov_count = m_ring_buffer.getReadIovecs(read_iovecs, 2);
        if (iov_count == 0) {
            m_has_buffered_frames = false;
            return false;
        }

        // 读取第一个帧头
        Http2FrameHeader header;
        if (read_iovecs[0].iov_len >= kHttp2FrameHeaderLength) {
            header = Http2FrameHeader::deserialize(
                static_cast<const uint8_t*>(read_iovecs[0].iov_base));
        } else {
            uint8_t header_buf[kHttp2FrameHeaderLength];
            size_t copied = 0;
            for (size_t i = 0; i < iov_count; ++i) {
                const auto& iov = read_iovecs[i];
                size_t to_copy = std::min(iov.iov_len, kHttp2FrameHeaderLength - copied);
                std::memcpy(header_buf + copied, iov.iov_base, to_copy);
                copied += to_copy;
                if (copied >= kHttp2FrameHeaderLength) break;
            }
            header = Http2FrameHeader::deserialize(header_buf);
        }

        if (header.length > m_peer_settings.max_frame_size) {
            setProtocolError(Http2ErrorCode::FrameSizeError, "frame too large");
            m_has_buffered_frames = true;
            return true;
        }

        size_t total_frame_size = kHttp2FrameHeaderLength + header.length;
        m_has_buffered_frames = (m_ring_buffer.readable() >= total_frame_size);
        return m_has_buffered_frames;
    }

    void setRecvError(const IOError& io_error) {
        if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
            if (m_peer_closed) {
                *m_peer_closed = true;
            }
        }
        if (m_last_error_msg) {
            *m_last_error_msg = io_error.message();
        }
        HTTP_LOG_DEBUG("[readFramesBatch] [recv-fail] [{}]", io_error.message());
        m_error = Http2ErrorCode::ProtocolError;
    }

    void setProtocolError(Http2ErrorCode code, const std::string& msg) {
        if (code == Http2ErrorCode::ProtocolError && msg == "peer closed" && m_peer_closed) {
            *m_peer_closed = true;
        }
        if (m_last_error_msg) {
            *m_last_error_msg = msg;
        }
        m_error = code;
    }

    std::expected<std::vector<Http2Frame::uptr>, Http2ErrorCode> parseFramesFromBuffer() {
        std::vector<Http2Frame::uptr> frames;
        const size_t reserve_hint =
            (m_max_frames == std::numeric_limits<size_t>::max())
                ? 16
                : std::min<size_t>(m_max_frames, 256);
        frames.reserve(reserve_hint);

        while (frames.size() < m_max_frames) {
            // 检查是否有足够的数据读取帧头
            if (m_ring_buffer.readable() < kHttp2FrameHeaderLength) {
                break;  // 不完整的帧头，停止解析
            }

            struct iovec read_iovecs[2];
            const size_t iov_count = m_ring_buffer.getReadIovecs(read_iovecs, 2);
            if (iov_count == 0) {
                break;
            }

            // 读取帧头
            Http2FrameHeader header;
            if (read_iovecs[0].iov_len >= kHttp2FrameHeaderLength) {
                header = Http2FrameHeader::deserialize(
                    static_cast<const uint8_t*>(read_iovecs[0].iov_base));
            } else {
                // 帧头跨越多个 iovec，需要拷贝
                uint8_t header_buf[kHttp2FrameHeaderLength];
                size_t copied = 0;
                for (size_t i = 0; i < iov_count; ++i) {
                    const auto& iov = read_iovecs[i];
                    size_t to_copy = std::min(iov.iov_len, kHttp2FrameHeaderLength - copied);
                    std::memcpy(header_buf + copied, iov.iov_base, to_copy);
                    copied += to_copy;
                    if (copied >= kHttp2FrameHeaderLength) break;
                }
                header = Http2FrameHeader::deserialize(header_buf);
            }

            // 验证帧大小
            if (header.length > m_peer_settings.max_frame_size) {
                return std::unexpected(Http2ErrorCode::FrameSizeError);
            }

            size_t total_frame_size = kHttp2FrameHeaderLength + header.length;

            // 检查是否有完整的帧
            if (m_ring_buffer.readable() < total_frame_size) {
                break;  // 不完整的帧，停止解析
            }

            // 解析完整帧
            std::expected<Http2Frame::uptr, Http2ErrorCode> frame_result;

            if (read_iovecs[0].iov_len >= total_frame_size) {
                // 帧在单个 iovec 中，直接解析
                frame_result = Http2FrameParser::parseFrame(
                    static_cast<const uint8_t*>(read_iovecs[0].iov_base), total_frame_size);
            } else {
                // 帧跨越多个 iovec，需要拷贝到临时缓冲区
                if (m_parse_buffer.size() < total_frame_size) {
                    m_parse_buffer.resize(total_frame_size);
                }
                size_t copied = 0;
                for (size_t i = 0; i < iov_count; ++i) {
                    const auto& iov = read_iovecs[i];
                    size_t to_copy = std::min(iov.iov_len, total_frame_size - copied);
                    std::memcpy(m_parse_buffer.data() + copied, iov.iov_base, to_copy);
                    copied += to_copy;
                    if (copied >= total_frame_size) break;
                }
                frame_result = Http2FrameParser::parseFrame(m_parse_buffer.data(), total_frame_size);
            }

            if (!frame_result) {
                return std::unexpected(frame_result.error());
            }

            // 消费已解析的帧数据
            m_ring_buffer.consume(total_frame_size);
            frames.push_back(std::move(*frame_result));
        }

        return frames;
    }

    RingBuffer& m_ring_buffer;
    Http2Settings& m_peer_settings;
    SocketType& m_socket;
    size_t m_max_frames;
    bool* m_peer_closed;
    std::string* m_last_error_msg;
    const bool* m_closing;
    bool m_has_buffered_frames;
    std::vector<uint8_t> m_parse_buffer;
    bool m_has_async_task;
    ProtocolReadvAwaitable m_recv_awaitable;
    std::optional<Http2ErrorCode> m_error;

public:
    std::expected<bool, IOError> m_result;
};

#ifdef GALAY_HTTP_SSL_ENABLED
// SslSocket 特化版本（CustomAwaitable + SslRecvAwaitable）
template<typename SocketType>
class Http2ReadFramesBatchAwaitableImpl<SocketType, true>
    : public CustomAwaitable
    , public TimeoutSupport<Http2ReadFramesBatchAwaitableImpl<SocketType, true>>
{
public:
    using RecvAwaitableType = galay::http::SslRecvCompatAwaitable;

    class ProtocolRecvAwaitable : public RecvAwaitableType
    {
    public:
        explicit ProtocolRecvAwaitable(Http2ReadFramesBatchAwaitableImpl* owner)
            : RecvAwaitableType(owner->m_socket.recv(owner->m_dummy_recv_buffer, sizeof(owner->m_dummy_recv_buffer)))
            , m_owner(owner)
        {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override {
            if (m_owner->m_closing && *m_owner->m_closing) {
                m_owner->setProtocolError(Http2ErrorCode::ProtocolError, "Connection closing");
                return true;
            }

            if (m_owner->checkBufferedFrames()) {
                return true;
            }

            if (!prepareRecvWindow()) {
                m_owner->setProtocolError(Http2ErrorCode::ProtocolError, "RingBuffer is full");
                return true;
            }

            if (cqe == nullptr) {
                return false;
            }

            this->m_sslResultSet = false;
            const bool done = RecvAwaitableType::handleComplete(cqe, handle);
            if (!done) {
                return false;
            }

            auto recv_result = std::move(this->m_sslResult);
            this->m_sslResultSet = false;

            if (!recv_result) {
                const auto& error = recv_result.error();
                if (error.sslError() == SSL_ERROR_WANT_READ || error.sslError() == SSL_ERROR_WANT_WRITE) {
                    return false;
                }
                m_owner->setSslRecvError(error);
                return true;
            }

            const size_t bytes_read = recv_result.value().size();
            if (bytes_read == 0) {
                m_owner->setProtocolError(Http2ErrorCode::ProtocolError, "peer closed");
                return true;
            }

            m_owner->m_ring_buffer.produce(bytes_read);
            if (m_owner->m_last_error_msg) {
                m_owner->m_last_error_msg->clear();
            }

            if (m_owner->checkBufferedFrames()) {
                return true;
            }

            if (!prepareRecvWindow()) {
                m_owner->setProtocolError(Http2ErrorCode::ProtocolError, "RingBuffer is full");
                return true;
            }
            return false;
        }
#else
        bool handleComplete(GHandle handle) override {
            if (m_owner->m_closing && *m_owner->m_closing) {
                m_owner->setProtocolError(Http2ErrorCode::ProtocolError, "Connection closing");
                return true;
            }

            if (m_owner->checkBufferedFrames()) {
                return true;
            }

            while (true) {
                if (!prepareRecvWindow()) {
                    m_owner->setProtocolError(Http2ErrorCode::ProtocolError, "RingBuffer is full");
                    return true;
                }

                this->m_sslResultSet = false;
                const bool done = RecvAwaitableType::handleComplete(handle);
                if (!done) {
                    return false;
                }

                auto recv_result = std::move(this->m_sslResult);
                this->m_sslResultSet = false;

                if (!recv_result) {
                    const auto& error = recv_result.error();
                    if (error.sslError() == SSL_ERROR_WANT_READ || error.sslError() == SSL_ERROR_WANT_WRITE) {
                        return false;
                    }
                    m_owner->setSslRecvError(error);
                    return true;
                }

                const size_t bytes_read = recv_result.value().size();
                if (bytes_read == 0) {
                    m_owner->setProtocolError(Http2ErrorCode::ProtocolError, "peer closed");
                    return true;
                }

                m_owner->m_ring_buffer.produce(bytes_read);
                if (m_owner->m_last_error_msg) {
                    m_owner->m_last_error_msg->clear();
                }

                if (m_owner->checkBufferedFrames()) {
                    return true;
                }
            }
        }
#endif

    private:
        bool prepareRecvWindow() {
            struct iovec write_iovecs[2];
            const size_t iov_count = m_owner->m_ring_buffer.getWriteIovecs(write_iovecs, 2);
            if (iov_count == 0) {
                return false;
            }
            this->m_plainBuffer = static_cast<char*>(write_iovecs[0].iov_base);
            this->m_plainLength = write_iovecs[0].iov_len;
            return this->m_plainLength > 0;
        }

        Http2ReadFramesBatchAwaitableImpl* m_owner;
    };

    Http2ReadFramesBatchAwaitableImpl(RingBuffer& ring_buffer,
                                      Http2Settings& peer_settings,
                                      SocketType& socket,
                                      size_t max_frames,
                                      bool* peer_closed = nullptr,
                                      std::string* last_error_msg = nullptr,
                                      const bool* closing = nullptr)
        : CustomAwaitable(socket.controller())
        , m_ring_buffer(ring_buffer)
        , m_peer_settings(peer_settings)
        , m_socket(socket)
        , m_max_frames(max_frames)
        , m_peer_closed(peer_closed)
        , m_last_error_msg(last_error_msg)
        , m_closing(closing)
        , m_has_buffered_frames(false)
        , m_has_async_task(false)
        , m_recv_awaitable(this)
        , m_result(true)
    {
        checkBufferedFrames();
        if (!(m_closing && *m_closing) && !m_has_buffered_frames) {
            addTask(IOEventType::RECV, &m_recv_awaitable);
            m_has_async_task = true;
        }
    }

    bool await_ready() const noexcept {
        if (m_closing && *m_closing) return true;
        return m_has_buffered_frames;
    }

    using CustomAwaitable::await_suspend;

    std::expected<std::vector<Http2Frame::uptr>, Http2ErrorCode> await_resume() {
        if (m_closing && *m_closing && !m_has_buffered_frames) {
            if (m_last_error_msg) {
                *m_last_error_msg = "Connection closing";
            }
            return std::unexpected(Http2ErrorCode::ProtocolError);
        }

        if (m_has_async_task) {
            onCompleted();

            if (!m_result.has_value()) {
                setRecvError(m_result.error());
            }
        }

        if (m_error.has_value()) {
            return std::unexpected(*m_error);
        }

        return parseFramesFromBuffer();
    }

private:
    bool checkBufferedFrames() {
        if (m_ring_buffer.readable() < kHttp2FrameHeaderLength) {
            m_has_buffered_frames = false;
            return false;
        }

        struct iovec read_iovecs[2];
        const size_t iov_count = m_ring_buffer.getReadIovecs(read_iovecs, 2);
        if (iov_count == 0) {
            m_has_buffered_frames = false;
            return false;
        }

        Http2FrameHeader header;
        if (read_iovecs[0].iov_len >= kHttp2FrameHeaderLength) {
            header = Http2FrameHeader::deserialize(
                static_cast<const uint8_t*>(read_iovecs[0].iov_base));
        } else {
            uint8_t header_buf[kHttp2FrameHeaderLength];
            size_t copied = 0;
            for (size_t i = 0; i < iov_count; ++i) {
                const auto& iov = read_iovecs[i];
                size_t to_copy = std::min(iov.iov_len, kHttp2FrameHeaderLength - copied);
                std::memcpy(header_buf + copied, iov.iov_base, to_copy);
                copied += to_copy;
                if (copied >= kHttp2FrameHeaderLength) break;
            }
            header = Http2FrameHeader::deserialize(header_buf);
        }
        if (header.length > m_peer_settings.max_frame_size) {
            setProtocolError(Http2ErrorCode::FrameSizeError, "frame too large");
            m_has_buffered_frames = true;
            return true;
        }
        size_t total_frame_size = kHttp2FrameHeaderLength + header.length;

        m_has_buffered_frames = (m_ring_buffer.readable() >= total_frame_size);
        return m_has_buffered_frames;
    }

    void setRecvError(const IOError& io_error) {
        if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
            if (m_peer_closed) {
                *m_peer_closed = true;
            }
        }
        if (m_last_error_msg) {
            *m_last_error_msg = io_error.message();
        }
        m_error = Http2ErrorCode::ProtocolError;
    }

    void setSslRecvError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            setProtocolError(Http2ErrorCode::ProtocolError, "peer closed");
            return;
        }
        setProtocolError(Http2ErrorCode::ProtocolError, error.message());
    }

    void setProtocolError(Http2ErrorCode code, const std::string& msg) {
        if (code == Http2ErrorCode::ProtocolError && msg == "peer closed" && m_peer_closed) {
            *m_peer_closed = true;
        }
        if (m_last_error_msg) {
            *m_last_error_msg = msg;
        }
        m_error = code;
    }

    std::expected<std::vector<Http2Frame::uptr>, Http2ErrorCode> parseFramesFromBuffer() {
        std::vector<Http2Frame::uptr> frames;
        const size_t reserve_hint =
            (m_max_frames == std::numeric_limits<size_t>::max())
                ? 16
                : std::min<size_t>(m_max_frames, 256);
        frames.reserve(reserve_hint);

        while (frames.size() < m_max_frames) {
            if (m_ring_buffer.readable() < kHttp2FrameHeaderLength) {
                break;
            }

            struct iovec read_iovecs[2];
            const size_t iov_count = m_ring_buffer.getReadIovecs(read_iovecs, 2);
            if (iov_count == 0) {
                break;
            }

            Http2FrameHeader header;
            if (read_iovecs[0].iov_len >= kHttp2FrameHeaderLength) {
                header = Http2FrameHeader::deserialize(
                    static_cast<const uint8_t*>(read_iovecs[0].iov_base));
            } else {
                uint8_t header_buf[kHttp2FrameHeaderLength];
                size_t copied = 0;
                for (size_t i = 0; i < iov_count; ++i) {
                    const auto& iov = read_iovecs[i];
                    size_t to_copy = std::min(iov.iov_len, kHttp2FrameHeaderLength - copied);
                    std::memcpy(header_buf + copied, iov.iov_base, to_copy);
                    copied += to_copy;
                    if (copied >= kHttp2FrameHeaderLength) break;
                }
                header = Http2FrameHeader::deserialize(header_buf);
            }

            if (header.length > m_peer_settings.max_frame_size) {
                return std::unexpected(Http2ErrorCode::FrameSizeError);
            }

            size_t total_frame_size = kHttp2FrameHeaderLength + header.length;

            if (m_ring_buffer.readable() < total_frame_size) {
                break;
            }

            std::expected<Http2Frame::uptr, Http2ErrorCode> frame_result;

            if (read_iovecs[0].iov_len >= total_frame_size) {
                frame_result = Http2FrameParser::parseFrame(
                    static_cast<const uint8_t*>(read_iovecs[0].iov_base), total_frame_size);
            } else {
                if (m_parse_buffer.size() < total_frame_size) {
                    m_parse_buffer.resize(total_frame_size);
                }
                size_t copied = 0;
                for (size_t i = 0; i < iov_count; ++i) {
                    const auto& iov = read_iovecs[i];
                    size_t to_copy = std::min(iov.iov_len, total_frame_size - copied);
                    std::memcpy(m_parse_buffer.data() + copied, iov.iov_base, to_copy);
                    copied += to_copy;
                    if (copied >= total_frame_size) break;
                }
                frame_result = Http2FrameParser::parseFrame(m_parse_buffer.data(), total_frame_size);
            }

            if (!frame_result) {
                return std::unexpected(frame_result.error());
            }

            m_ring_buffer.consume(total_frame_size);
            frames.push_back(std::move(*frame_result));
        }

        return frames;
    }

    RingBuffer& m_ring_buffer;
    Http2Settings& m_peer_settings;
    SocketType& m_socket;
    size_t m_max_frames;
    bool* m_peer_closed;
    std::string* m_last_error_msg;
    const bool* m_closing;
    bool m_has_buffered_frames;
    std::vector<uint8_t> m_parse_buffer;
    bool m_has_async_task;
    char m_dummy_recv_buffer[1];
    ProtocolRecvAwaitable m_recv_awaitable;
    std::optional<Http2ErrorCode> m_error;

public:
    std::expected<bool, IOError> m_result;
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
        , m_peer_closed(false)
        , m_closing(false)
        , m_expecting_continuation(false)
        , m_continuation_stream_id(0)
        , m_is_client(false)
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
        , m_peer_closed(false)
        , m_closing(false)
        , m_expecting_continuation(false)
        , m_continuation_stream_id(0)
        , m_is_client(false)
    {
        // 升级后需要扩展 buffer 大小以适应 HTTP/2
        if (m_ring_buffer.capacity() < 65536) {
            // 保留已有数据，扩展容量
            RingBuffer new_buffer(65536);
            // 复制已有数据到新 buffer
            auto read_iovecs = m_ring_buffer.getReadIovecs();
            for (const auto& iov : read_iovecs) {
                size_t remaining = iov.iov_len;
                const char* src = static_cast<const char*>(iov.iov_base);
                while (remaining > 0) {
                    auto write_iovecs = new_buffer.getWriteIovecs();
                    if (write_iovecs.empty()) break;
                    for (const auto& wv : write_iovecs) {
                        if (remaining == 0) break;
                        size_t to_copy = std::min(remaining, wv.iov_len);
                        std::memcpy(wv.iov_base, src, to_copy);
                        new_buffer.produce(to_copy);
                        src += to_copy;
                        remaining -= to_copy;
                    }
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
        , m_peer_closed(false)
        , m_closing(false)
        , m_expecting_continuation(false)
        , m_continuation_stream_id(0)
        , m_is_client(false)
    {
    }

    ~Http2ConnImpl();

    // 禁用拷贝
    Http2ConnImpl(const Http2ConnImpl&) = delete;
    Http2ConnImpl& operator=(const Http2ConnImpl&) = delete;

    // 启用移动
    Http2ConnImpl(Http2ConnImpl&&) noexcept;
    Http2ConnImpl& operator=(Http2ConnImpl&&) noexcept;
    
    // 获取 socket
    SocketType& socket() { return m_socket; }
    
    // 获取本地/对端设置
    Http2Settings& localSettings() { return m_local_settings; }
    Http2Settings& peerSettings() { return m_peer_settings; }
    Http2RuntimeConfig& runtimeConfig() { return m_runtime_config; }
    const Http2RuntimeConfig& runtimeConfig() const { return m_runtime_config; }
    
    // HPACK 编解码器
    HpackEncoder& encoder() { return m_encoder; }
    HpackDecoder& decoder() { return m_decoder; }
    
    // 流管理
    Http2Stream::ptr getStream(uint32_t stream_id) {
        auto it = m_streams.find(stream_id);
        return it != m_streams.end() ? it->second : nullptr;
    }
    
    Http2Stream::ptr createStream(uint32_t stream_id) {
        auto [it, inserted] = m_streams.try_emplace(stream_id);
        if (inserted || !it->second) {
            it->second = Http2Stream::create(stream_id);
        }
        return it->second;
    }
    
    void removeStream(uint32_t stream_id) {
        m_streams.erase(stream_id);
    }

    void reserveStreams(size_t capacity) {
        if (capacity == 0) {
            return;
        }
        if (capacity > m_streams.bucket_count()) {
            m_streams.reserve(capacity);
        }
    }
    
    size_t streamCount() const { return m_streams.size(); }

    // 遍历所有流
    template<typename Func>
    void forEachStream(Func&& func) {
        for (auto& [id, stream] : m_streams) {
            func(id, stream);
        }
    }

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
    Http2FlowControlUpdate evaluateRecvWindowUpdate(int32_t stream_recv_window, size_t data_size) const {
        uint32_t target = m_runtime_config.flow_control_target_window == 0
            ? m_local_settings.initial_window_size
            : m_runtime_config.flow_control_target_window;
        if (target == 0) {
            target = kDefaultInitialWindowSize;
        }

        if (m_runtime_config.flow_control_strategy) {
            return m_runtime_config.flow_control_strategy(
                m_conn_recv_window, stream_recv_window, target, data_size);
        }

        Http2FlowControlUpdate update;
        const int32_t low_watermark = static_cast<int32_t>(target / 2);
        if (m_conn_recv_window < low_watermark) {
            update.conn_increment = static_cast<uint32_t>(target - m_conn_recv_window);
        }
        if (stream_recv_window < low_watermark) {
            update.stream_increment = static_cast<uint32_t>(target - stream_recv_window);
        }
        return update;
    }
    
    // 客户端/服务端模式
    bool isClient() const { return m_is_client; }
    void setIsClient(bool is_client) { m_is_client = is_client; }

    // GOAWAY 状态
    bool isGoawaySent() const { return m_goaway_sent; }
    bool isGoawayReceived() const { return m_goaway_received; }
    void setGoawaySent() { m_goaway_sent = true; }
    void setGoawayReceived() { m_goaway_received = true; }
    void markGoawayReceived(uint32_t last_stream_id,
                            Http2ErrorCode error_code,
                            std::string debug = "") {
        m_goaway_received = true;
        m_draining = true;
        m_goaway_last_stream_id = last_stream_id;
        m_goaway_error_code = error_code;
        m_goaway_debug_data = std::move(debug);
    }
    void markGoawaySent(uint32_t last_stream_id,
                        Http2ErrorCode error_code,
                        std::string debug = "") {
        m_goaway_sent = true;
        m_draining = true;
        m_goaway_last_stream_id = last_stream_id;
        m_goaway_error_code = error_code;
        m_goaway_debug_data = std::move(debug);
    }
    bool isDraining() const { return m_draining; }
    void setDraining(bool draining) { m_draining = draining; }
    uint32_t goawayLastStreamId() const { return m_goaway_last_stream_id; }
    Http2ErrorCode goawayErrorCode() const { return m_goaway_error_code; }
    const std::string& goawayDebugData() const { return m_goaway_debug_data; }

    void markSettingsSent() {
        m_settings_ack_pending = true;
        m_settings_sent_at = std::chrono::steady_clock::now();
    }
    void markSettingsAckReceived() { m_settings_ack_pending = false; }
    bool isSettingsAckPending() const { return m_settings_ack_pending; }
    std::chrono::steady_clock::time_point settingsSentAt() const { return m_settings_sent_at; }

    bool isPeerClosed() const { return m_peer_closed; }
    bool isClosing() const { return m_closing; }
    const std::string& lastReadError() const { return m_last_read_error; }
    
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
    
    // 关闭连接（awaitable 版本，需要 co_await）。
    // 只负责传输层 teardown；协议级清理由 StreamManager 负责。
    auto close() {
        m_closing = true;
        // shutdown(fd) 触发读事件（readv 返回 0），让 readerLoop 退出阻塞读取。
        const int fd = m_socket.handle().fd;
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
        }
        return m_socket.close();
    }

    // 非 awaitable 关闭：仅设置 closing 标志并触发 TCP shutdown，
    // 用于唤醒 readerLoop；不执行协议级收尾。
    void initiateClose() {
        m_closing = true;
        const int fd = m_socket.handle().fd;
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
        }
    }

    // StreamManager 访问（需要 include Http2StreamManager.h 后才能使用）
    Http2StreamManagerImpl<SocketType>* streamManager() { return m_stream_manager.get(); }
    void initStreamManager() {
        if (!m_stream_manager) {
            m_stream_manager = std::make_unique<Http2StreamManagerImpl<SocketType>>(*this);
        }
    }

    Http2ConnectionCore* connectionCore() { return m_connection_core.get(); }
    Http2ConnectionCore& ensureConnectionCore() {
        if (!m_connection_core) {
            m_connection_core = std::make_unique<Http2ConnectionCore>();
        }
        return *m_connection_core;
    }

    /**
     * @brief 获取接收缓冲区引用
     */
    RingBuffer& ringBuffer() { return m_ring_buffer; }

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

    /**
     * @brief 解析缓冲区中已有的完整帧（批量）
     * @param max_count 最多解析的帧数量（默认无限制）
     * @return 成功时返回帧向量，失败时返回错误码
     * @details
     * - 仅解析已缓冲的完整帧，不执行任何 socket recv
     * - 遇到不完整的尾部帧时停止（不报错）
     * - 验证帧头 length <= peerSettings().max_frame_size
     * - 返回 FrameSizeError 如果帧过大
     */
    std::expected<std::vector<Http2Frame::uptr>, Http2ErrorCode>
    parseBufferedFrames(size_t max_count = std::numeric_limits<size_t>::max()) {
        std::vector<Http2Frame::uptr> frames;

        while (frames.size() < max_count) {
            // 检查是否有足够的数据读取帧头
            if (m_ring_buffer.readable() < kHttp2FrameHeaderLength) {
                break;  // 不完整的帧头，停止解析
            }

            auto read_iovecs = m_ring_buffer.getReadIovecs();
            if (read_iovecs.empty()) {
                break;
            }

            // 读取帧头
            Http2FrameHeader header;
            if (read_iovecs[0].iov_len >= kHttp2FrameHeaderLength) {
                header = Http2FrameHeader::deserialize(
                    static_cast<const uint8_t*>(read_iovecs[0].iov_base));
            } else {
                // 帧头跨越多个 iovec，需要拷贝
                uint8_t header_buf[kHttp2FrameHeaderLength];
                size_t copied = 0;
                for (const auto& iov : read_iovecs) {
                    size_t to_copy = std::min(iov.iov_len, kHttp2FrameHeaderLength - copied);
                    std::memcpy(header_buf + copied, iov.iov_base, to_copy);
                    copied += to_copy;
                    if (copied >= kHttp2FrameHeaderLength) break;
                }
                header = Http2FrameHeader::deserialize(header_buf);
            }

            // 验证帧大小
            if (header.length > m_peer_settings.max_frame_size) {
                return std::unexpected(Http2ErrorCode::FrameSizeError);
            }

            size_t total_frame_size = kHttp2FrameHeaderLength + header.length;

            // 检查是否有完整的帧
            if (m_ring_buffer.readable() < total_frame_size) {
                break;  // 不完整的帧，停止解析
            }

            // 解析完整帧
            std::expected<Http2Frame::uptr, Http2ErrorCode> frame_result;

            if (read_iovecs[0].iov_len >= total_frame_size) {
                // 帧在单个 iovec 中，直接解析
                frame_result = Http2FrameParser::parseFrame(
                    static_cast<const uint8_t*>(read_iovecs[0].iov_base), total_frame_size);
            } else {
                // 帧跨越多个 iovec，需要拷贝到临时缓冲区
                if (m_parse_buffer.size() < total_frame_size) {
                    m_parse_buffer.resize(total_frame_size);
                }
                size_t copied = 0;
                for (const auto& iov : read_iovecs) {
                    size_t to_copy = std::min(iov.iov_len, total_frame_size - copied);
                    std::memcpy(m_parse_buffer.data() + copied, iov.iov_base, to_copy);
                    copied += to_copy;
                    if (copied >= total_frame_size) break;
                }
                frame_result = Http2FrameParser::parseFrame(m_parse_buffer.data(), total_frame_size);
            }

            if (!frame_result) {
                return std::unexpected(frame_result.error());
            }

            // 消费已解析的帧数据
            m_ring_buffer.consume(total_frame_size);
            frames.push_back(std::move(*frame_result));
        }

        return frames;
    }

    // ==================== 帧读写（返回 Awaitable） ====================
    
    /**
     * @brief 获取帧读取等待体
     */
    Http2ReadFrameAwaitableImpl<SocketType> readFrame() {
        return Http2ReadFrameAwaitableImpl<SocketType>(m_ring_buffer, m_peer_settings, m_socket,
                                                       &m_peer_closed, &m_last_read_error, &m_closing);
    }

    /**
     * @brief 获取批量帧读取等待体
     */
    Http2ReadFramesBatchAwaitableImpl<SocketType> readFramesBatch(
        size_t max_frames = std::numeric_limits<size_t>::max()) {
        return Http2ReadFramesBatchAwaitableImpl<SocketType>(
            m_ring_buffer, m_peer_settings, m_socket, max_frames,
            &m_peer_closed, &m_last_read_error, &m_closing);
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
        markSettingsSent();
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
    Http2WriteFrameAwaitableImpl<SocketType> sendGoaway(Http2ErrorCode error,
                                                        const std::string& debug = "",
                                                        std::optional<uint32_t> last_stream_id = std::nullopt) {
        Http2GoAwayFrame frame;
        uint32_t last = last_stream_id.value_or(m_last_peer_stream_id);
        frame.setLastStreamId(last);
        frame.setErrorCode(error);
        frame.setDebugData(debug);
        markGoawaySent(last, error, debug);
        return writeFrame(frame);
    }
    
    /**
     * @brief 发送 RST_STREAM
     */
    Http2WriteFrameAwaitableImpl<SocketType> sendRstStream(uint32_t stream_id, Http2ErrorCode error) {
        auto bytes = Http2FrameBuilder::rstStreamBytes(stream_id, error);
        
        auto stream = getStream(stream_id);
        if (stream) {
            stream->onRstStreamSent();
        }
        
        return writeRaw(std::move(bytes));
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
        auto bytes = Http2FrameBuilder::headersBytes(stream_id,
                                                     header_block,
                                                     end_stream,
                                                     end_headers);
        
        auto stream = getStream(stream_id);
        if (stream) {
            stream->onHeadersSent(end_stream);
        }
        
        return writeRaw(std::move(bytes));
    }
    
    /**
     * @brief 发送 DATA 帧（单帧）
     */
    Http2WriteFrameAwaitableImpl<SocketType> sendDataFrame(
        uint32_t stream_id,
        const std::string& data,
        bool end_stream = false)
    {
        auto bytes = Http2FrameBuilder::dataBytes(stream_id, data, end_stream);
        
        auto stream = getStream(stream_id);
        if (stream) {
            m_conn_send_window -= data.size();
            stream->adjustSendWindow(-static_cast<int32_t>(data.size()));
            if (end_stream) {
                stream->onDataSent(true);
            }
        }
        
        return writeRaw(std::move(bytes));
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

    struct PushPromisePrepareResult {
        uint32_t promised_stream_id;
        Http2WriteFrameAwaitableImpl<SocketType> send_awaitable;
    };
    
    /**
     * @brief 创建推送流并准备 PUSH_PROMISE
     * @return 推送准备结果；如果推送被禁用返回 nullopt
     */
    std::optional<PushPromisePrepareResult> preparePushPromise(
        uint32_t stream_id,
        const std::string& method,
        const std::string& path,
        const std::string& authority,
        const std::string& scheme = "http")
    {
        if (!m_peer_settings.enable_push) {
            return std::nullopt;
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
        
        return PushPromisePrepareResult{
            promised_stream_id,
            sendPushPromise(stream_id, promised_stream_id, headers)
        };
    }

private:
    SocketType m_socket;
    RingBuffer m_ring_buffer;
    std::vector<uint8_t> m_parse_buffer;  // 用于跨 iovec 边界的帧解析

    // 连接设置
    Http2Settings m_local_settings;
    Http2Settings m_peer_settings;
    Http2RuntimeConfig m_runtime_config;
    
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
    bool m_draining = false;
    uint32_t m_goaway_last_stream_id = 0;
    Http2ErrorCode m_goaway_error_code = Http2ErrorCode::NoError;
    std::string m_goaway_debug_data;
    bool m_settings_ack_pending = false;
    std::chrono::steady_clock::time_point m_settings_sent_at{};
    bool m_is_client;
    bool m_peer_closed;
    bool m_closing;
    std::string m_last_read_error;

    // CONTINUATION 状态
    bool m_expecting_continuation;
    uint32_t m_continuation_stream_id;

    // StreamManager
    std::unique_ptr<Http2StreamManagerImpl<SocketType>> m_stream_manager;
    std::unique_ptr<Http2ConnectionCore> m_connection_core;
};

// 类型别名
using Http2Conn = Http2ConnImpl<galay::async::TcpSocket>;
using Http2ReadFrameAwaitable = Http2ReadFrameAwaitableImpl<galay::async::TcpSocket>;
using Http2ReadFramesBatchAwaitable = Http2ReadFramesBatchAwaitableImpl<galay::async::TcpSocket>;
using Http2WriteFrameAwaitable = Http2WriteFrameAwaitableImpl<galay::async::TcpSocket>;

#ifdef GALAY_HTTP_SSL_ENABLED
using Http2sConn = Http2ConnImpl<galay::ssl::SslSocket>;
using Http2sReadFrameAwaitable = Http2ReadFrameAwaitableImpl<galay::ssl::SslSocket>;
using Http2sReadFramesBatchAwaitable = Http2ReadFramesBatchAwaitableImpl<galay::ssl::SslSocket>;
using Http2sWriteFrameAwaitable = Http2WriteFrameAwaitableImpl<galay::ssl::SslSocket>;
#endif

} // namespace galay::http2

// Http2StreamManager 的完整定义（解决 unique_ptr 析构需要完整类型的问题）
#include "Http2StreamManager.h"

namespace galay::http2
{

template<typename SocketType>
Http2ConnImpl<SocketType>::~Http2ConnImpl() = default;

template<typename SocketType>
Http2ConnImpl<SocketType>::Http2ConnImpl(Http2ConnImpl&&) noexcept = default;

template<typename SocketType>
Http2ConnImpl<SocketType>& Http2ConnImpl<SocketType>::operator=(Http2ConnImpl&&) noexcept = default;

} // namespace galay::http2

#endif // GALAY_HTTP2_CONN_H
