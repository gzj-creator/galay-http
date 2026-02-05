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
#include "galay-ssl/SslSocket.h"
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

// TcpSocket 特化版本（使用 readv）
template<typename SocketType>
class GetFrameAwaitableImpl<SocketType, false> : public galay::kernel::TimeoutSupport<GetFrameAwaitableImpl<SocketType, false>>
{
public:
    using ReadvAwaitableType = decltype(std::declval<SocketType>().readv(std::declval<std::vector<iovec>>()));

    GetFrameAwaitableImpl(RingBuffer& ring_buffer,
                         const WsReaderSetting& setting,
                         WsFrame& frame,
                         ReadvAwaitableType&& readv_awaitable,
                         bool is_server)
        : m_ring_buffer(&ring_buffer)
        , m_setting(&setting)
        , m_frame(&frame)
        , m_readv_awaitable(std::move(readv_awaitable))
        , m_is_server(is_server)
        , m_total_received(0)
        , m_has_buffered_frame(checkBufferedFrame())
    {
    }

    bool await_ready() const noexcept {
        return m_has_buffered_frame;
    }

    auto await_suspend(std::coroutine_handle<> handle) {
        return m_readv_awaitable.await_suspend(handle);
    }

    std::expected<bool, WsError> await_resume() {
        if (m_has_buffered_frame) {
            return parseFromBuffer();
        }

        auto result = m_readv_awaitable.await_resume();

        if (!result.has_value()) {
            return std::unexpected(WsError(kWsConnectionClosed, "Socket readv failed"));
        }

        size_t bytes_received = result.value();
        if (bytes_received == 0) {
            return std::unexpected(WsError(kWsConnectionClosed, "Connection closed by peer"));
        }

        m_total_received += bytes_received;
        m_ring_buffer->produce(bytes_received);

        return parseFromBuffer();
    }

private:
    bool checkBufferedFrame() const {
        auto iovecs = m_ring_buffer->getReadIovecs();
        if (iovecs.empty()) {
            return false;
        }

        WsFrame frame;
        auto parse_result = WsFrameParser::fromIOVec(iovecs, frame, m_is_server);
        if (!parse_result.has_value()) {
            return parse_result.error().code() != kWsIncomplete;
        }

        return true;
    }

    std::expected<bool, WsError> parseFromBuffer() {
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
                    return std::unexpected(WsError(kWsMessageTooLarge, "Frame size exceeds limit"));
                }
                return false;
            }

            return std::unexpected(error);
        }

        size_t consumed = parse_result.value();
        m_ring_buffer->consume(consumed);

        if (m_frame->header.payload_length > m_setting->max_frame_size) {
            return std::unexpected(WsError(kWsMessageTooLarge, "Frame payload too large"));
        }

        return true;
    }

    RingBuffer* m_ring_buffer;
    const WsReaderSetting* m_setting;
    WsFrame* m_frame;
    ReadvAwaitableType m_readv_awaitable;
    bool m_is_server;
    size_t m_total_received;
    bool m_has_buffered_frame;

public:
    std::expected<bool, galay::kernel::IOError> m_result;
};

#ifdef GALAY_HTTP_SSL_ENABLED
// SslSocket 特化版本（使用 recv）
template<typename SocketType>
class GetFrameAwaitableImpl<SocketType, true> : public galay::kernel::TimeoutSupport<GetFrameAwaitableImpl<SocketType, true>>
{
public:
    using RecvAwaitableType = decltype(std::declval<SocketType>().recv(std::declval<char*>(), std::declval<size_t>()));

    GetFrameAwaitableImpl(RingBuffer& ring_buffer,
                         const WsReaderSetting& setting,
                         WsFrame& frame,
                         SocketType& socket,
                         bool is_server)
        : m_ring_buffer(&ring_buffer)
        , m_setting(&setting)
        , m_frame(&frame)
        , m_socket(&socket)
        , m_is_server(is_server)
        , m_total_received(0)
        , m_has_buffered_frame(checkBufferedFrame())
    {
    }

    bool await_ready() const noexcept {
        return m_has_buffered_frame;
    }

    auto await_suspend(std::coroutine_handle<> handle) {
        if (!m_recv_awaitable) {
            auto write_iovecs = m_ring_buffer->getWriteIovecs();
            if (!write_iovecs.empty()) {
                m_recv_awaitable.emplace(m_socket->recv(
                    static_cast<char*>(write_iovecs[0].iov_base),
                    write_iovecs[0].iov_len));
            }
        }
        if (!m_recv_awaitable) {
            return false;  // 不挂起，直接返回
        }
        return m_recv_awaitable->await_suspend(handle);
    }

    std::expected<bool, WsError> await_resume() {
        if (m_has_buffered_frame) {
            return parseFromBuffer();
        }

        if (!m_recv_awaitable) {
            return std::unexpected(WsError(kWsConnectionError, "Ring buffer has no space for writing"));
        }

        auto result = m_recv_awaitable->await_resume();
        m_recv_awaitable.reset();

        if (!result.has_value()) {
            return std::unexpected(WsError(kWsConnectionClosed, "Socket recv failed"));
        }

        size_t bytes_received = result.value().size();
        if (bytes_received == 0) {
            return std::unexpected(WsError(kWsConnectionClosed, "Connection closed by peer"));
        }

        m_total_received += bytes_received;
        m_ring_buffer->produce(bytes_received);

        return parseFromBuffer();
    }

private:
    bool checkBufferedFrame() const {
        auto iovecs = m_ring_buffer->getReadIovecs();
        if (iovecs.empty()) {
            return false;
        }

        WsFrame temp_frame;
        auto parse_result = WsFrameParser::fromIOVec(iovecs, temp_frame, m_is_server);
        return parse_result.has_value();
    }

    std::expected<bool, WsError> parseFromBuffer() {
        auto iovecs = m_ring_buffer->getReadIovecs();
        if (iovecs.empty()) {
            return false;
        }

        auto parse_result = WsFrameParser::fromIOVec(iovecs, *m_frame, m_is_server);

        if (!parse_result.has_value()) {
            WsError error = parse_result.error();
            if (error.code() == kWsIncomplete) {
                if (m_total_received >= m_setting->max_frame_size) {
                    return std::unexpected(WsError(kWsMessageTooLarge, "Frame size exceeds limit"));
                }
                return false;
            }
            return std::unexpected(error);
        }

        size_t consumed = parse_result.value();
        m_ring_buffer->consume(consumed);

        if (m_frame->header.payload_length > m_setting->max_frame_size) {
            return std::unexpected(WsError(kWsMessageTooLarge, "Frame payload too large"));
        }

        return true;
    }

    RingBuffer* m_ring_buffer;
    const WsReaderSetting* m_setting;
    WsFrame* m_frame;
    SocketType* m_socket;
    std::optional<RecvAwaitableType> m_recv_awaitable;
    bool m_is_server;
    size_t m_total_received;
    bool m_has_buffered_frame;

public:
    std::expected<bool, galay::kernel::IOError> m_result;
};
#endif

/**
 * @brief WebSocket消息读取等待体模板类
 */
template<typename SocketType, bool IsSsl = is_ssl_socket_v<SocketType>>
class GetMessageAwaitableImpl;

// TcpSocket 特化版本（使用 readv）
template<typename SocketType>
class GetMessageAwaitableImpl<SocketType, false> : public galay::kernel::TimeoutSupport<GetMessageAwaitableImpl<SocketType, false>>
{
public:
    using ReadvAwaitableType = decltype(std::declval<SocketType>().readv(std::declval<std::vector<iovec>>()));

    GetMessageAwaitableImpl(RingBuffer& ring_buffer,
                           const WsReaderSetting& setting,
                           std::string& message,
                           WsOpcode& opcode,
                           ReadvAwaitableType&& readv_awaitable,
                           bool is_server,
                           SocketType& socket,
                           bool use_mask,
                           ControlFrameCallback control_frame_callback = nullptr)
        : m_ring_buffer(&ring_buffer)
        , m_setting(&setting)
        , m_message(&message)
        , m_opcode(&opcode)
        , m_readv_awaitable(std::move(readv_awaitable))
        , m_is_server(is_server)
        , m_socket(&socket)
        , m_use_mask(use_mask)
        , m_total_received(0)
        , m_first_frame(true)
        , m_control_frame_callback(control_frame_callback)
        , m_has_buffered_message(checkBufferedMessage())
    {
    }

    bool await_ready() const noexcept {
        return m_has_buffered_message;
    }

    auto await_suspend(std::coroutine_handle<> handle) {
        return m_readv_awaitable.await_suspend(handle);
    }

    std::expected<bool, WsError> await_resume() {
        if (m_has_buffered_message) {
            return parseFromBuffer();
        }

        auto result = m_readv_awaitable.await_resume();

        if (!result.has_value()) {
            return std::unexpected(WsError(kWsConnectionClosed, "Socket readv failed"));
        }

        size_t bytes_received = result.value();
        if (bytes_received == 0) {
            return std::unexpected(WsError(kWsConnectionClosed, "Connection closed by peer"));
        }

        m_total_received += bytes_received;
        m_ring_buffer->produce(bytes_received);

        return parseFromBuffer();
    }

private:
    static std::vector<iovec> sliceIovecs(const std::vector<iovec>& iovecs, size_t offset) {
        std::vector<iovec> out;
        size_t current_offset = offset;

        for (const auto& iov : iovecs) {
            if (current_offset >= iov.iov_len) {
                current_offset -= iov.iov_len;
                continue;
            }

            iovec sliced = iov;
            sliced.iov_base = static_cast<char*>(iov.iov_base) + current_offset;
            sliced.iov_len = iov.iov_len - current_offset;
            out.push_back(sliced);
            current_offset = 0;
        }

        return out;
    }

    bool checkBufferedMessage() const {
        auto iovecs = m_ring_buffer->getReadIovecs();
        if (iovecs.empty()) {
            return false;
        }

        size_t total = 0;
        for (const auto& iov : iovecs) {
            total += iov.iov_len;
        }

        size_t offset = 0;
        bool first_frame = true;

        while (offset < total) {
            auto sliced = sliceIovecs(iovecs, offset);
            if (sliced.empty()) {
                return false;
            }

            WsFrame frame;
            auto parse_result = WsFrameParser::fromIOVec(sliced, frame, m_is_server);

            if (!parse_result.has_value()) {
                return parse_result.error().code() != kWsIncomplete;
            }

            offset += parse_result.value();

            if (isControlFrame(frame.header.opcode)) {
                return true;
            }

            if (first_frame) {
                if (frame.header.opcode == WsOpcode::Continuation) {
                    return true;
                }
                first_frame = false;
            } else {
                if (frame.header.opcode != WsOpcode::Continuation) {
                    return true;
                }
            }

            if (frame.header.fin) {
                return true;
            }
        }

        return false;
    }

    std::expected<bool, WsError> parseFromBuffer() {
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
                        return std::unexpected(WsError(kWsMessageTooLarge, "Message size exceeds limit"));
                    }
                    return false;
                }

                return std::unexpected(error);
            }

            size_t consumed = parse_result.value();
            m_ring_buffer->consume(consumed);

            if (isControlFrame(frame.header.opcode)) {
                if (!frame.header.fin) {
                    return std::unexpected(WsError(kWsControlFrameFragmented));
                }

                *m_message = frame.payload;
                *m_opcode = frame.header.opcode;
                return true;
            }

            if (m_first_frame) {
                if (frame.header.opcode == WsOpcode::Continuation) {
                    return std::unexpected(WsError(kWsProtocolError, "First frame cannot be continuation"));
                }
                *m_opcode = frame.header.opcode;
                m_first_frame = false;
            } else {
                if (frame.header.opcode != WsOpcode::Continuation) {
                    return std::unexpected(WsError(kWsProtocolError, "Expected continuation frame"));
                }
            }

            *m_message += frame.payload;

            if (m_message->size() > m_setting->max_message_size) {
                return std::unexpected(WsError(kWsMessageTooLarge, "Message size exceeds limit"));
            }

            if (frame.header.fin) {
                return true;
            }

            if (m_ring_buffer->getReadIovecs().empty()) {
                return false;
            }
        }
    }

    RingBuffer* m_ring_buffer;
    const WsReaderSetting* m_setting;
    std::string* m_message;
    WsOpcode* m_opcode;
    ReadvAwaitableType m_readv_awaitable;
    bool m_is_server;
    SocketType* m_socket;
    bool m_use_mask;
    size_t m_total_received;
    bool m_first_frame;
    ControlFrameCallback m_control_frame_callback;
    bool m_has_buffered_message;

public:
    std::expected<bool, galay::kernel::IOError> m_result;
};

#ifdef GALAY_HTTP_SSL_ENABLED
// SslSocket 特化版本（使用 recv）
template<typename SocketType>
class GetMessageAwaitableImpl<SocketType, true> : public galay::kernel::TimeoutSupport<GetMessageAwaitableImpl<SocketType, true>>
{
public:
    using RecvAwaitableType = decltype(std::declval<SocketType>().recv(std::declval<char*>(), std::declval<size_t>()));

    GetMessageAwaitableImpl(RingBuffer& ring_buffer,
                           const WsReaderSetting& setting,
                           std::string& message,
                           WsOpcode& opcode,
                           SocketType& socket,
                           bool is_server,
                           bool use_mask,
                           ControlFrameCallback control_frame_callback = nullptr)
        : m_ring_buffer(&ring_buffer)
        , m_setting(&setting)
        , m_message(&message)
        , m_opcode(&opcode)
        , m_socket(&socket)
        , m_is_server(is_server)
        , m_use_mask(use_mask)
        , m_total_received(0)
        , m_first_frame(true)
        , m_control_frame_callback(control_frame_callback)
        , m_has_buffered_message(checkBufferedMessage())
    {
    }

    bool await_ready() const noexcept {
        return m_has_buffered_message;
    }

    auto await_suspend(std::coroutine_handle<> handle) {
        if (!m_recv_awaitable) {
            auto write_iovecs = m_ring_buffer->getWriteIovecs();
            if (!write_iovecs.empty()) {
                m_recv_awaitable.emplace(m_socket->recv(
                    static_cast<char*>(write_iovecs[0].iov_base),
                    write_iovecs[0].iov_len));
            }
        }
        if (!m_recv_awaitable) {
            return false;  // 不挂起，直接返回
        }
        return m_recv_awaitable->await_suspend(handle);
    }

    std::expected<bool, WsError> await_resume() {
        if (m_has_buffered_message) {
            return parseFromBuffer();
        }

        if (!m_recv_awaitable) {
            return std::unexpected(WsError(kWsConnectionError, "Ring buffer has no space for writing"));
        }

        auto result = m_recv_awaitable->await_resume();
        m_recv_awaitable.reset();

        if (!result.has_value()) {
            return std::unexpected(WsError(kWsConnectionClosed, "Socket recv failed"));
        }

        size_t bytes_received = result.value().size();
        if (bytes_received == 0) {
            return std::unexpected(WsError(kWsConnectionClosed, "Connection closed by peer"));
        }

        m_total_received += bytes_received;
        m_ring_buffer->produce(bytes_received);

        return parseFromBuffer();
    }

private:
    static std::vector<iovec> sliceIovecs(const std::vector<iovec>& iovecs, size_t offset) {
        std::vector<iovec> out;
        size_t current_offset = offset;

        for (const auto& iov : iovecs) {
            if (current_offset >= iov.iov_len) {
                current_offset -= iov.iov_len;
                continue;
            }

            iovec sliced = iov;
            sliced.iov_base = static_cast<char*>(iov.iov_base) + current_offset;
            sliced.iov_len = iov.iov_len - current_offset;
            out.push_back(sliced);
            current_offset = 0;
        }

        return out;
    }

    bool checkBufferedMessage() const {
        auto iovecs = m_ring_buffer->getReadIovecs();
        if (iovecs.empty()) {
            return false;
        }

        size_t total = 0;
        for (const auto& iov : iovecs) {
            total += iov.iov_len;
        }

        size_t offset = 0;
        bool first_frame = true;

        while (offset < total) {
            auto sliced = sliceIovecs(iovecs, offset);
            if (sliced.empty()) {
                return false;
            }

            WsFrame frame;
            auto parse_result = WsFrameParser::fromIOVec(sliced, frame, m_is_server);

            if (!parse_result.has_value()) {
                return parse_result.error().code() != kWsIncomplete;
            }

            offset += parse_result.value();

            if (isControlFrame(frame.header.opcode)) {
                return true;
            }

            if (first_frame) {
                if (frame.header.opcode == WsOpcode::Continuation) {
                    return true;
                }
                first_frame = false;
            } else {
                if (frame.header.opcode != WsOpcode::Continuation) {
                    return true;
                }
            }

            if (frame.header.fin) {
                return true;
            }
        }

        return false;
    }

    std::expected<bool, WsError> parseFromBuffer() {
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
                        return std::unexpected(WsError(kWsMessageTooLarge, "Message size exceeds limit"));
                    }
                    return false;
                }

                return std::unexpected(error);
            }

            size_t consumed = parse_result.value();
            m_ring_buffer->consume(consumed);

            if (isControlFrame(frame.header.opcode)) {
                if (!frame.header.fin) {
                    return std::unexpected(WsError(kWsControlFrameFragmented));
                }

                *m_message = frame.payload;
                *m_opcode = frame.header.opcode;
                return true;
            }

            if (m_first_frame) {
                if (frame.header.opcode == WsOpcode::Continuation) {
                    return std::unexpected(WsError(kWsProtocolError, "First frame cannot be continuation"));
                }
                *m_opcode = frame.header.opcode;
                m_first_frame = false;
            } else {
                if (frame.header.opcode != WsOpcode::Continuation) {
                    return std::unexpected(WsError(kWsProtocolError, "Expected continuation frame"));
                }
            }

            m_message->append(frame.payload);

            if (m_message->size() > m_setting->max_message_size) {
                return std::unexpected(WsError(kWsMessageTooLarge, "Message size exceeds limit"));
            }

            if (frame.header.fin) {
                return true;
            }
        }
    }

    RingBuffer* m_ring_buffer;
    const WsReaderSetting* m_setting;
    std::string* m_message;
    WsOpcode* m_opcode;
    std::optional<RecvAwaitableType> m_recv_awaitable;
    bool m_is_server;
    SocketType* m_socket;
    bool m_use_mask;
    size_t m_total_received;
    bool m_first_frame;
    ControlFrameCallback m_control_frame_callback;
    bool m_has_buffered_message;

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
        if constexpr (is_ssl_socket_v<SocketType>) {
            return GetFrameAwaitableImpl<SocketType>(*m_ring_buffer, m_setting, frame,
                                    *m_socket, m_is_server);
        } else {
            return GetFrameAwaitableImpl<SocketType>(*m_ring_buffer, m_setting, frame,
                                    m_socket->readv(m_ring_buffer->getWriteIovecs()),
                                    m_is_server);
        }
    }

    GetMessageAwaitableImpl<SocketType> getMessage(std::string& message, WsOpcode& opcode) {
        if constexpr (is_ssl_socket_v<SocketType>) {
            return GetMessageAwaitableImpl<SocketType>(*m_ring_buffer, m_setting, message, opcode,
                                      *m_socket, m_is_server, m_use_mask);
        } else {
            return GetMessageAwaitableImpl<SocketType>(*m_ring_buffer, m_setting, message, opcode,
                                      m_socket->readv(m_ring_buffer->getWriteIovecs()),
                                      m_is_server, *m_socket, m_use_mask);
        }
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
#include "galay-ssl/SslSocket.h"
namespace galay::websocket {
using GetFrameAwaitableSsl = GetFrameAwaitableImpl<galay::ssl::SslSocket>;
using GetMessageAwaitableSsl = GetMessageAwaitableImpl<galay::ssl::SslSocket>;
using WssReader = WsReaderImpl<galay::ssl::SslSocket>;
} // namespace galay::websocket
#endif

#endif // GALAY_WS_READER_H
