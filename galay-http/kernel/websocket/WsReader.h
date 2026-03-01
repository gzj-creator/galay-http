#ifndef GALAY_WS_READER_H
#define GALAY_WS_READER_H

#include "WsReaderSetting.h"
#include "galay-http/protoc/websocket/WebSocketError.h"
#include "galay-http/protoc/websocket/WebSocketFrame.h"
#include "galay-kernel/common/Buffer.h"
#include "galay-kernel/kernel/Awaitable.h"
#include "galay-kernel/kernel/Timeout.hpp"
#include "galay-kernel/async/TcpSocket.h"
#include <expected>
#include <coroutine>
#include <functional>

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/async/SslSocket.h"
#include "galay-http/kernel/SslRecvCompatAwaitable.h"
#endif

namespace galay::websocket
{

using namespace galay::kernel;
using namespace galay::async;

// 类型特征：检测是否是 SslSocket
template<typename T>
struct is_ssl_socket : std::false_type {};

#ifdef GALAY_HTTP_SSL_ENABLED
template<>
struct is_ssl_socket<galay::ssl::SslSocket> : std::true_type {};
#endif

template<typename T>
inline constexpr bool is_ssl_socket_v = is_ssl_socket<T>::value;

using ControlFrameCallback = std::function<void(WsOpcode opcode, const std::string& payload)>;

// 前向声明
template<typename SocketType>
class WsReaderImpl;

/**
 * @brief WebSocket帧读取等待体模板类
 */
template<typename SocketType, bool IsSsl = is_ssl_socket_v<SocketType>>
class GetFrameAwaitableImpl;

// TcpSocket 特化版本（CustomAwaitable + RecvAwaitable）
template<typename SocketType>
class GetFrameAwaitableImpl<SocketType, false>
    : public galay::kernel::CustomAwaitable
    , public galay::kernel::TimeoutSupport<GetFrameAwaitableImpl<SocketType, false>>
{
public:
    class ProtocolRecvAwaitable : public galay::kernel::RecvAwaitable
    {
    public:
        explicit ProtocolRecvAwaitable(GetFrameAwaitableImpl* owner)
            : galay::kernel::RecvAwaitable(owner->m_socket->controller(), nullptr, 0)
            , m_owner(owner)
        {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle) override {
            if (m_owner->parseFromBuffer()) {
                return true;
            }

            if (cqe == nullptr) {
                if (!prepareRecvWindow()) {
                    m_owner->setParseError(WsError(kWsConnectionError, "Ring buffer has no space for writing"));
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
                m_owner->setParseError(WsError(kWsConnectionClosed, "Connection closed by peer"));
                return true;
            }

            m_owner->m_total_received += recv_bytes;
            m_owner->m_ring_buffer->produce(recv_bytes);

            if (m_owner->parseFromBuffer()) {
                return true;
            }

            if (!prepareRecvWindow()) {
                m_owner->setParseError(WsError(kWsConnectionError, "Ring buffer has no space for writing"));
                return true;
            }
            return false;
        }
#else
        bool handleComplete(GHandle handle) override {
            if (m_owner->parseFromBuffer()) {
                return true;
            }

            while (true) {
                if (!prepareRecvWindow()) {
                    m_owner->setParseError(WsError(kWsConnectionError, "Ring buffer has no space for writing"));
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
                    m_owner->setParseError(WsError(kWsConnectionClosed, "Connection closed by peer"));
                    return true;
                }

                m_owner->m_total_received += recv_bytes;
                m_owner->m_ring_buffer->produce(recv_bytes);

                if (m_owner->parseFromBuffer()) {
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

        GetFrameAwaitableImpl* m_owner;
    };

    GetFrameAwaitableImpl(RingBuffer& ring_buffer,
                         const WsReaderSetting& setting,
                         WsFrame& frame,
                         SocketType& socket,
                         bool is_server)
        : galay::kernel::CustomAwaitable(socket.controller())
        , m_ring_buffer(&ring_buffer)
        , m_setting(&setting)
        , m_frame(&frame)
        , m_socket(&socket)
        , m_is_server(is_server)
        , m_total_received(0)
        , m_has_buffered_frame(false)
        , m_recv_awaitable(this)
        , m_result(true)
    {
        m_has_buffered_frame = parseFromBuffer();
        if (!m_has_buffered_frame) {
            addTask(IOEventType::RECV, &m_recv_awaitable);
        }
    }

    bool await_ready() const noexcept {
        return m_has_buffered_frame;
    }

    using galay::kernel::CustomAwaitable::await_suspend;

    std::expected<bool, WsError> await_resume() {
        if (m_has_buffered_frame) {
            if (m_ws_error.has_value()) {
                return std::unexpected(std::move(*m_ws_error));
            }
            return true;
        }

        onCompleted();
        if (!m_result.has_value()) {
            auto& io_error = m_result.error();
            if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
                return std::unexpected(WsError(kWsConnectionClosed, io_error.message()));
            }
            return std::unexpected(WsError(kWsConnectionError, io_error.message()));
        }

        if (m_ws_error.has_value()) {
            return std::unexpected(std::move(*m_ws_error));
        }

        return true;
    }

private:
    bool parseFromBuffer() {
        auto iovecs = m_ring_buffer->getReadIovecs();
        if (iovecs.empty()) {
            return false;
        }

        auto parse_result = WsFrameParser::fromIOVec(iovecs, *m_frame, m_is_server);
        if (!parse_result.has_value()) {
            WsError error = parse_result.error();
            if (error.code() == kWsIncomplete) {
                size_t buffered = m_ring_buffer->readable();
                if (m_total_received + buffered > m_setting->max_frame_size) {
                    setParseError(WsError(kWsMessageTooLarge, "Frame size exceeds limit"));
                    return true;
                }
                return false;
            }
            setParseError(std::move(error));
            return true;
        }

        size_t consumed = parse_result.value();
        m_ring_buffer->consume(consumed);

        if (m_frame->header.payload_length > m_setting->max_frame_size) {
            setParseError(WsError(kWsMessageTooLarge, "Frame payload too large"));
            return true;
        }

        return true;
    }

    void setRecvError(const galay::kernel::IOError& io_error) {
        if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
            m_ws_error = WsError(kWsConnectionClosed, io_error.message());
            return;
        }
        m_ws_error = WsError(kWsConnectionError, io_error.message());
    }

    void setParseError(WsError&& error) {
        m_ws_error = std::move(error);
    }

    RingBuffer* m_ring_buffer;
    const WsReaderSetting* m_setting;
    WsFrame* m_frame;
    SocketType* m_socket;
    bool m_is_server;
    size_t m_total_received;
    bool m_has_buffered_frame;
    ProtocolRecvAwaitable m_recv_awaitable;
    std::optional<WsError> m_ws_error;

public:
    std::expected<bool, galay::kernel::IOError> m_result;
};

#ifdef GALAY_HTTP_SSL_ENABLED
// SslSocket 特化版本（CustomAwaitable + SslRecvAwaitable）
template<typename SocketType>
class GetFrameAwaitableImpl<SocketType, true>
    : public galay::kernel::CustomAwaitable
    , public galay::kernel::TimeoutSupport<GetFrameAwaitableImpl<SocketType, true>>
{
public:
    using RecvAwaitableType = galay::http::SslRecvCompatAwaitable;

    class ProtocolRecvAwaitable : public RecvAwaitableType
    {
    public:
        explicit ProtocolRecvAwaitable(GetFrameAwaitableImpl* owner)
            : RecvAwaitableType(owner->m_socket->recv(owner->m_dummy_recv_buffer, sizeof(owner->m_dummy_recv_buffer)))
            , m_owner(owner)
        {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override {
            if (m_owner->parseFromBuffer()) {
                return true;
            }

            if (!prepareRecvWindow()) {
                m_owner->setParseError(WsError(kWsConnectionError, "Ring buffer has no space for writing"));
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

            const size_t recv_bytes = recv_result.value().size();
            if (recv_bytes == 0) {
                m_owner->setParseError(WsError(kWsConnectionClosed, "Connection closed by peer"));
                return true;
            }

            m_owner->m_total_received += recv_bytes;
            m_owner->m_ring_buffer->produce(recv_bytes);
            return m_owner->parseFromBuffer();
        }
#else
        bool handleComplete(GHandle handle) override {
            if (m_owner->parseFromBuffer()) {
                return true;
            }

            while (true) {
                if (!prepareRecvWindow()) {
                    m_owner->setParseError(WsError(kWsConnectionError, "Ring buffer has no space for writing"));
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

                const size_t recv_bytes = recv_result.value().size();
                if (recv_bytes == 0) {
                    m_owner->setParseError(WsError(kWsConnectionClosed, "Connection closed by peer"));
                    return true;
                }

                m_owner->m_total_received += recv_bytes;
                m_owner->m_ring_buffer->produce(recv_bytes);
                if (m_owner->parseFromBuffer()) {
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
            this->m_plainBuffer = static_cast<char*>(write_iovecs[0].iov_base);
            this->m_plainLength = write_iovecs[0].iov_len;
            return this->m_plainLength > 0;
        }

        GetFrameAwaitableImpl* m_owner;
    };

    GetFrameAwaitableImpl(RingBuffer& ring_buffer,
                         const WsReaderSetting& setting,
                         WsFrame& frame,
                         SocketType& socket,
                         bool is_server)
        : galay::kernel::CustomAwaitable(socket.controller())
        , m_ring_buffer(&ring_buffer)
        , m_setting(&setting)
        , m_frame(&frame)
        , m_socket(&socket)
        , m_is_server(is_server)
        , m_total_received(0)
        , m_has_buffered_frame(false)
        , m_has_async_task(false)
        , m_recv_awaitable(this)
        , m_result(true)
    {
        m_has_buffered_frame = parseFromBuffer();
        if (!m_has_buffered_frame) {
            addTask(IOEventType::RECV, &m_recv_awaitable);
            m_has_async_task = true;
        }
    }

    bool await_ready() const noexcept {
        return m_has_buffered_frame;
    }

    using galay::kernel::CustomAwaitable::await_suspend;

    std::expected<bool, WsError> await_resume() {
        if (m_has_buffered_frame) {
            if (m_ws_error.has_value()) {
                return std::unexpected(std::move(*m_ws_error));
            }
            return true;
        }

        if (m_has_async_task) {
            onCompleted();
            if (!m_result.has_value()) {
                setRecvError(m_result.error());
            }
        }

        if (m_ws_error.has_value()) {
            return std::unexpected(std::move(*m_ws_error));
        }

        return true;
    }

private:
    bool parseFromBuffer() {
        auto iovecs = m_ring_buffer->getReadIovecs();
        if (iovecs.empty()) {
            return false;
        }

        auto parse_result = WsFrameParser::fromIOVec(iovecs, *m_frame, m_is_server);
        if (!parse_result.has_value()) {
            WsError error = parse_result.error();
            if (error.code() == kWsIncomplete) {
                size_t buffered = m_ring_buffer->readable();
                if (m_total_received + buffered > m_setting->max_frame_size) {
                    setParseError(WsError(kWsMessageTooLarge, "Frame size exceeds limit"));
                    return true;
                }
                return false;
            }
            setParseError(std::move(error));
            return true;
        }

        size_t consumed = parse_result.value();
        m_ring_buffer->consume(consumed);

        if (m_frame->header.payload_length > m_setting->max_frame_size) {
            setParseError(WsError(kWsMessageTooLarge, "Frame payload too large"));
            return true;
        }

        return true;
    }

    void setRecvError(const galay::kernel::IOError& io_error) {
        if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
            m_ws_error = WsError(kWsConnectionClosed, io_error.message());
            return;
        }
        m_ws_error = WsError(kWsConnectionError, io_error.message());
    }

    void setSslRecvError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            m_ws_error = WsError(kWsConnectionClosed, "Connection closed by peer");
            return;
        }
        m_ws_error = WsError(kWsConnectionError, error.message());
    }

    void setParseError(WsError&& error) {
        m_ws_error = std::move(error);
    }

    RingBuffer* m_ring_buffer;
    const WsReaderSetting* m_setting;
    WsFrame* m_frame;
    SocketType* m_socket;
    bool m_is_server;
    size_t m_total_received;
    bool m_has_buffered_frame;
    bool m_has_async_task;
    char m_dummy_recv_buffer[1];
    ProtocolRecvAwaitable m_recv_awaitable;
    std::optional<WsError> m_ws_error;

public:
    std::expected<bool, galay::kernel::IOError> m_result;
};
#endif

/**
 * @brief WebSocket消息读取等待体模板类
 */
template<typename SocketType, bool IsSsl = is_ssl_socket_v<SocketType>>
class GetMessageAwaitableImpl;

// TcpSocket 特化版本（CustomAwaitable + RecvAwaitable）
template<typename SocketType>
class GetMessageAwaitableImpl<SocketType, false>
    : public galay::kernel::CustomAwaitable
    , public galay::kernel::TimeoutSupport<GetMessageAwaitableImpl<SocketType, false>>
{
public:
    class ProtocolRecvAwaitable : public galay::kernel::RecvAwaitable
    {
    public:
        explicit ProtocolRecvAwaitable(GetMessageAwaitableImpl* owner)
            : galay::kernel::RecvAwaitable(owner->m_socket->controller(), nullptr, 0)
            , m_owner(owner)
        {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle) override {
            if (m_owner->parseFromBuffer()) {
                return true;
            }

            if (cqe == nullptr) {
                if (!prepareRecvWindow()) {
                    m_owner->setParseError(WsError(kWsConnectionError, "Ring buffer has no space for writing"));
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
                m_owner->setParseError(WsError(kWsConnectionClosed, "Connection closed by peer"));
                return true;
            }

            m_owner->m_total_received += recv_bytes;
            m_owner->m_ring_buffer->produce(recv_bytes);

            if (m_owner->parseFromBuffer()) {
                return true;
            }

            if (!prepareRecvWindow()) {
                m_owner->setParseError(WsError(kWsConnectionError, "Ring buffer has no space for writing"));
                return true;
            }
            return false;
        }
#else
        bool handleComplete(GHandle handle) override {
            if (m_owner->parseFromBuffer()) {
                return true;
            }

            while (true) {
                if (!prepareRecvWindow()) {
                    m_owner->setParseError(WsError(kWsConnectionError, "Ring buffer has no space for writing"));
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
                    m_owner->setParseError(WsError(kWsConnectionClosed, "Connection closed by peer"));
                    return true;
                }

                m_owner->m_total_received += recv_bytes;
                m_owner->m_ring_buffer->produce(recv_bytes);

                if (m_owner->parseFromBuffer()) {
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

        GetMessageAwaitableImpl* m_owner;
    };

    GetMessageAwaitableImpl(RingBuffer& ring_buffer,
                           const WsReaderSetting& setting,
                           std::string& message,
                           WsOpcode& opcode,
                           SocketType& socket,
                           bool is_server,
                           bool use_mask,
                           ControlFrameCallback control_frame_callback = nullptr)
        : galay::kernel::CustomAwaitable(socket.controller())
        , m_ring_buffer(&ring_buffer)
        , m_setting(&setting)
        , m_message(&message)
        , m_opcode(&opcode)
        , m_is_server(is_server)
        , m_socket(&socket)
        , m_use_mask(use_mask)
        , m_total_received(0)
        , m_first_frame(true)
        , m_control_frame_callback(control_frame_callback)
        , m_has_buffered_message(false)
        , m_recv_awaitable(this)
        , m_result(true)
    {
        m_has_buffered_message = parseFromBuffer();
        if (!m_has_buffered_message) {
            addTask(IOEventType::RECV, &m_recv_awaitable);
        }
    }

    bool await_ready() const noexcept {
        return m_has_buffered_message;
    }

    using galay::kernel::CustomAwaitable::await_suspend;

    std::expected<bool, WsError> await_resume() {
        if (m_has_buffered_message) {
            if (m_ws_error.has_value()) {
                return std::unexpected(std::move(*m_ws_error));
            }
            return true;
        }

        onCompleted();
        if (!m_result.has_value()) {
            auto& io_error = m_result.error();
            if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
                return std::unexpected(WsError(kWsConnectionClosed, io_error.message()));
            }
            return std::unexpected(WsError(kWsConnectionError, io_error.message()));
        }

        if (m_ws_error.has_value()) {
            return std::unexpected(std::move(*m_ws_error));
        }

        return true;
    }

private:
    bool parseFromBuffer() {
        while (true) {
            auto iovecs = m_ring_buffer->getReadIovecs();
            if (iovecs.empty()) {
                return false;
            }

            WsFrame frame;
            auto parse_result = WsFrameParser::fromIOVec(iovecs, frame, m_is_server);

            if (!parse_result.has_value()) {
                WsError error = parse_result.error();

                if (error.code() == kWsIncomplete) {
                    size_t buffered = m_ring_buffer->readable();
                    if (m_message->size() + m_total_received + buffered > m_setting->max_message_size) {
                        setParseError(WsError(kWsMessageTooLarge, "Message size exceeds limit"));
                        return true;
                    }
                    return false;
                }

                setParseError(std::move(error));
                return true;
            }

            size_t consumed = parse_result.value();
            m_ring_buffer->consume(consumed);

            if (isControlFrame(frame.header.opcode)) {
                if (!frame.header.fin) {
                    setParseError(WsError(kWsControlFrameFragmented));
                    return true;
                }

                *m_message = std::move(frame.payload);
                *m_opcode = frame.header.opcode;
                return true;
            }

            if (m_first_frame) {
                if (frame.header.opcode == WsOpcode::Continuation) {
                    setParseError(WsError(kWsProtocolError, "First frame cannot be continuation"));
                    return true;
                }
                *m_opcode = frame.header.opcode;
                m_first_frame = false;

                // Hot path: most messages are single-frame; move payload to avoid an extra copy.
                *m_message = std::move(frame.payload);
            } else {
                if (frame.header.opcode != WsOpcode::Continuation) {
                    setParseError(WsError(kWsProtocolError, "Expected continuation frame"));
                    return true;
                }
                m_message->append(frame.payload);
            }

            if (m_message->size() > m_setting->max_message_size) {
                setParseError(WsError(kWsMessageTooLarge, "Message size exceeds limit"));
                return true;
            }

            if (frame.header.fin) {
                return true;
            }

            if (m_ring_buffer->getReadIovecs().empty()) {
                return false;
            }
        }
    }

    void setRecvError(const galay::kernel::IOError& io_error) {
        if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
            m_ws_error = WsError(kWsConnectionClosed, io_error.message());
            return;
        }
        m_ws_error = WsError(kWsConnectionError, io_error.message());
    }

    void setParseError(WsError&& error) {
        m_ws_error = std::move(error);
    }

    RingBuffer* m_ring_buffer;
    const WsReaderSetting* m_setting;
    std::string* m_message;
    WsOpcode* m_opcode;
    bool m_is_server;
    SocketType* m_socket;
    bool m_use_mask;
    size_t m_total_received;
    bool m_first_frame;
    ControlFrameCallback m_control_frame_callback;
    bool m_has_buffered_message;
    ProtocolRecvAwaitable m_recv_awaitable;
    std::optional<WsError> m_ws_error;

public:
    std::expected<bool, galay::kernel::IOError> m_result;
};

#ifdef GALAY_HTTP_SSL_ENABLED
// SslSocket 特化版本（CustomAwaitable + SslRecvAwaitable）
template<typename SocketType>
class GetMessageAwaitableImpl<SocketType, true>
    : public galay::kernel::CustomAwaitable
    , public galay::kernel::TimeoutSupport<GetMessageAwaitableImpl<SocketType, true>>
{
public:
    using RecvAwaitableType = galay::http::SslRecvCompatAwaitable;

    class ProtocolRecvAwaitable : public RecvAwaitableType
    {
    public:
        explicit ProtocolRecvAwaitable(GetMessageAwaitableImpl* owner)
            : RecvAwaitableType(owner->m_socket->recv(owner->m_dummy_recv_buffer, sizeof(owner->m_dummy_recv_buffer)))
            , m_owner(owner)
        {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override {
            if (m_owner->parseFromBuffer()) {
                return true;
            }

            if (!prepareRecvWindow()) {
                m_owner->setParseError(WsError(kWsConnectionError, "Ring buffer has no space for writing"));
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

            const size_t recv_bytes = recv_result.value().size();
            if (recv_bytes == 0) {
                m_owner->setParseError(WsError(kWsConnectionClosed, "Connection closed by peer"));
                return true;
            }

            m_owner->m_total_received += recv_bytes;
            m_owner->m_ring_buffer->produce(recv_bytes);
            return m_owner->parseFromBuffer();
        }
#else
        bool handleComplete(GHandle handle) override {
            if (m_owner->parseFromBuffer()) {
                return true;
            }

            while (true) {
                if (!prepareRecvWindow()) {
                    m_owner->setParseError(WsError(kWsConnectionError, "Ring buffer has no space for writing"));
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

                const size_t recv_bytes = recv_result.value().size();
                if (recv_bytes == 0) {
                    m_owner->setParseError(WsError(kWsConnectionClosed, "Connection closed by peer"));
                    return true;
                }

                m_owner->m_total_received += recv_bytes;
                m_owner->m_ring_buffer->produce(recv_bytes);
                if (m_owner->parseFromBuffer()) {
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
            this->m_plainBuffer = static_cast<char*>(write_iovecs[0].iov_base);
            this->m_plainLength = write_iovecs[0].iov_len;
            return this->m_plainLength > 0;
        }

        GetMessageAwaitableImpl* m_owner;
    };

    GetMessageAwaitableImpl(RingBuffer& ring_buffer,
                           const WsReaderSetting& setting,
                           std::string& message,
                           WsOpcode& opcode,
                           SocketType& socket,
                           bool is_server,
                           bool use_mask,
                           ControlFrameCallback control_frame_callback = nullptr)
        : galay::kernel::CustomAwaitable(socket.controller())
        , m_ring_buffer(&ring_buffer)
        , m_setting(&setting)
        , m_message(&message)
        , m_opcode(&opcode)
        , m_is_server(is_server)
        , m_socket(&socket)
        , m_use_mask(use_mask)
        , m_total_received(0)
        , m_first_frame(true)
        , m_control_frame_callback(control_frame_callback)
        , m_has_buffered_message(false)
        , m_has_async_task(false)
        , m_recv_awaitable(this)
        , m_result(true)
    {
        m_has_buffered_message = parseFromBuffer();
        if (!m_has_buffered_message) {
            addTask(IOEventType::RECV, &m_recv_awaitable);
            m_has_async_task = true;
        }
    }

    bool await_ready() const noexcept {
        return m_has_buffered_message;
    }

    using galay::kernel::CustomAwaitable::await_suspend;

    std::expected<bool, WsError> await_resume() {
        if (m_has_buffered_message) {
            if (m_ws_error.has_value()) {
                return std::unexpected(std::move(*m_ws_error));
            }
            return true;
        }

        if (m_has_async_task) {
            onCompleted();
            if (!m_result.has_value()) {
                setRecvError(m_result.error());
            }
        }

        if (m_ws_error.has_value()) {
            return std::unexpected(std::move(*m_ws_error));
        }

        return true;
    }

private:
    bool parseFromBuffer() {
        while (true) {
            auto iovecs = m_ring_buffer->getReadIovecs();
            if (iovecs.empty()) {
                return false;
            }

            WsFrame frame;
            auto parse_result = WsFrameParser::fromIOVec(iovecs, frame, m_is_server);

            if (!parse_result.has_value()) {
                WsError error = parse_result.error();

                if (error.code() == kWsIncomplete) {
                    size_t buffered = m_ring_buffer->readable();
                    if (m_message->size() + m_total_received + buffered > m_setting->max_message_size) {
                        setParseError(WsError(kWsMessageTooLarge, "Message size exceeds limit"));
                        return true;
                    }
                    return false;
                }

                setParseError(std::move(error));
                return true;
            }

            size_t consumed = parse_result.value();
            m_ring_buffer->consume(consumed);

            if (isControlFrame(frame.header.opcode)) {
                if (!frame.header.fin) {
                    setParseError(WsError(kWsControlFrameFragmented));
                    return true;
                }

                *m_message = std::move(frame.payload);
                *m_opcode = frame.header.opcode;
                return true;
            }

            if (m_first_frame) {
                if (frame.header.opcode == WsOpcode::Continuation) {
                    setParseError(WsError(kWsProtocolError, "First frame cannot be continuation"));
                    return true;
                }
                *m_opcode = frame.header.opcode;
                m_first_frame = false;

                // Hot path: most messages are single-frame; move payload to avoid an extra copy.
                *m_message = std::move(frame.payload);
            } else {
                if (frame.header.opcode != WsOpcode::Continuation) {
                    setParseError(WsError(kWsProtocolError, "Expected continuation frame"));
                    return true;
                }
                m_message->append(frame.payload);
            }

            if (m_message->size() > m_setting->max_message_size) {
                setParseError(WsError(kWsMessageTooLarge, "Message size exceeds limit"));
                return true;
            }

            if (frame.header.fin) {
                return true;
            }

            if (m_ring_buffer->getReadIovecs().empty()) {
                return false;
            }
        }
    }

    void setRecvError(const galay::kernel::IOError& io_error) {
        if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
            m_ws_error = WsError(kWsConnectionClosed, io_error.message());
            return;
        }
        m_ws_error = WsError(kWsConnectionError, io_error.message());
    }

    void setSslRecvError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            m_ws_error = WsError(kWsConnectionClosed, "Connection closed by peer");
            return;
        }
        m_ws_error = WsError(kWsConnectionError, error.message());
    }

    void setParseError(WsError&& error) {
        m_ws_error = std::move(error);
    }

    RingBuffer* m_ring_buffer;
    const WsReaderSetting* m_setting;
    std::string* m_message;
    WsOpcode* m_opcode;
    bool m_is_server;
    SocketType* m_socket;
    bool m_use_mask;
    size_t m_total_received;
    bool m_first_frame;
    ControlFrameCallback m_control_frame_callback;
    bool m_has_buffered_message;
    bool m_has_async_task;
    char m_dummy_recv_buffer[1];
    ProtocolRecvAwaitable m_recv_awaitable;
    std::optional<WsError> m_ws_error;

public:
    std::expected<bool, galay::kernel::IOError> m_result;
};
#endif

/**
 * @brief WebSocket读取器模板类
 */
template<typename SocketType>
class WsReaderImpl
{
public:
    WsReaderImpl(RingBuffer& ring_buffer, const WsReaderSetting& setting, SocketType& socket, bool is_server = true, bool use_mask = false)
        : m_ring_buffer(&ring_buffer)
        , m_setting(setting)
        , m_socket(&socket)
        , m_is_server(is_server)
        , m_use_mask(use_mask)
    {
    }

    GetFrameAwaitableImpl<SocketType> getFrame(WsFrame& frame) {
        return GetFrameAwaitableImpl<SocketType>(*m_ring_buffer, m_setting, frame,
                                *m_socket, m_is_server);
    }

    GetMessageAwaitableImpl<SocketType> getMessage(std::string& message, WsOpcode& opcode) {
        return GetMessageAwaitableImpl<SocketType>(*m_ring_buffer, m_setting, message, opcode,
                                  *m_socket, m_is_server, m_use_mask);
    }

private:
    RingBuffer* m_ring_buffer;
    WsReaderSetting m_setting;
    SocketType* m_socket;
    bool m_is_server;
    bool m_use_mask;
};

// 类型别名 - WebSocket over TCP
using GetFrameAwaitable = GetFrameAwaitableImpl<TcpSocket>;
using GetMessageAwaitable = GetMessageAwaitableImpl<TcpSocket>;
using WsReader = WsReaderImpl<TcpSocket>;

} // namespace galay::websocket

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/async/SslSocket.h"
namespace galay::websocket {
using GetFrameAwaitableSsl = GetFrameAwaitableImpl<galay::ssl::SslSocket>;
using GetMessageAwaitableSsl = GetMessageAwaitableImpl<galay::ssl::SslSocket>;
using WssReader = WsReaderImpl<galay::ssl::SslSocket>;
} // namespace galay::websocket
#endif

#endif // GALAY_WS_READER_H
