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

namespace galay::websocket
{

using namespace galay::kernel;
using namespace galay::async;

using ControlFrameCallback = std::function<void(WsOpcode opcode, const std::string& payload)>;

// 前向声明
template<typename SocketType>
class WsReaderImpl;

/**
 * @brief WebSocket帧读取等待体模板类
 */
template<typename SocketType>
class GetFrameAwaitableImpl : public galay::kernel::TimeoutSupport<GetFrameAwaitableImpl<SocketType>>
{
public:
    using ReadvAwaitableType = decltype(std::declval<SocketType>().readv(std::declval<std::vector<iovec>>()));

    GetFrameAwaitableImpl(RingBuffer& ring_buffer,
                         const WsReaderSetting& setting,
                         WsFrame& frame,
                         ReadvAwaitableType&& readv_awaitable,
                         bool is_server)
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_frame(frame)
        , m_readv_awaitable(std::move(readv_awaitable))
        , m_is_server(is_server)
        , m_total_received(0)
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    auto await_suspend(std::coroutine_handle<> handle) {
        return m_readv_awaitable.await_suspend(handle);
    }

    std::expected<bool, WsError> await_resume() {
        auto result = m_readv_awaitable.await_resume();

        if (!result.has_value()) {
            return std::unexpected(WsError(kWsConnectionClosed, "Socket readv failed"));
        }

        size_t bytes_received = result.value();
        if (bytes_received == 0) {
            return std::unexpected(WsError(kWsConnectionClosed, "Connection closed by peer"));
        }

        m_total_received += bytes_received;
        m_ring_buffer.produce(bytes_received);

        auto iovecs = m_ring_buffer.getReadIovecs();
        auto parse_result = WsFrameParser::fromIOVec(iovecs, m_frame, m_is_server);

        if (!parse_result.has_value()) {
            WsError error = parse_result.error();

            if (error.code() == kWsIncomplete) {
                if (m_total_received > m_setting.max_frame_size) {
                    return std::unexpected(WsError(kWsMessageTooLarge, "Frame size exceeds limit"));
                }
                return false;
            }

            return std::unexpected(error);
        }

        size_t consumed = parse_result.value();
        m_ring_buffer.consume(consumed);

        if (m_frame.header.payload_length > m_setting.max_frame_size) {
            return std::unexpected(WsError(kWsMessageTooLarge, "Frame payload too large"));
        }

        return true;
    }

private:
    RingBuffer& m_ring_buffer;
    const WsReaderSetting& m_setting;
    WsFrame& m_frame;
    ReadvAwaitableType m_readv_awaitable;
    bool m_is_server;
    size_t m_total_received;

public:
    std::expected<bool, galay::kernel::IOError> m_result;
};

/**
 * @brief WebSocket消息读取等待体模板类
 */
template<typename SocketType>
class GetMessageAwaitableImpl : public galay::kernel::TimeoutSupport<GetMessageAwaitableImpl<SocketType>>
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
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_message(message)
        , m_opcode(opcode)
        , m_readv_awaitable(std::move(readv_awaitable))
        , m_is_server(is_server)
        , m_socket(socket)
        , m_use_mask(use_mask)
        , m_total_received(0)
        , m_first_frame(true)
        , m_control_frame_callback(control_frame_callback)
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    auto await_suspend(std::coroutine_handle<> handle) {
        return m_readv_awaitable.await_suspend(handle);
    }

    std::expected<bool, WsError> await_resume() {
        auto result = m_readv_awaitable.await_resume();

        if (!result.has_value()) {
            return std::unexpected(WsError(kWsConnectionClosed, "Socket readv failed"));
        }

        size_t bytes_received = result.value();
        if (bytes_received == 0) {
            return std::unexpected(WsError(kWsConnectionClosed, "Connection closed by peer"));
        }

        m_total_received += bytes_received;
        m_ring_buffer.produce(bytes_received);

        while (true) {
            auto iovecs = m_ring_buffer.getReadIovecs();
            if (iovecs.empty()) {
                return false;
            }

            WsFrame frame;
            auto parse_result = WsFrameParser::fromIOVec(iovecs, frame, m_is_server);

            if (!parse_result.has_value()) {
                WsError error = parse_result.error();

                if (error.code() == kWsIncomplete) {
                    if (m_message.size() + m_total_received > m_setting.max_message_size) {
                        return std::unexpected(WsError(kWsMessageTooLarge, "Message size exceeds limit"));
                    }
                    return false;
                }

                return std::unexpected(error);
            }

            size_t consumed = parse_result.value();
            m_ring_buffer.consume(consumed);

            if (isControlFrame(frame.header.opcode)) {
                if (!frame.header.fin) {
                    return std::unexpected(WsError(kWsControlFrameFragmented));
                }

                m_message = frame.payload;
                m_opcode = frame.header.opcode;
                return true;
            }

            if (m_first_frame) {
                if (frame.header.opcode == WsOpcode::Continuation) {
                    return std::unexpected(WsError(kWsProtocolError, "First frame cannot be continuation"));
                }
                m_opcode = frame.header.opcode;
                m_first_frame = false;
            } else {
                if (frame.header.opcode != WsOpcode::Continuation) {
                    return std::unexpected(WsError(kWsProtocolError, "Expected continuation frame"));
                }
            }

            m_message += frame.payload;

            if (m_message.size() > m_setting.max_message_size) {
                return std::unexpected(WsError(kWsMessageTooLarge, "Message size exceeds limit"));
            }

            if (frame.header.fin) {
                return true;
            }

            if (m_ring_buffer.getReadIovecs().empty()) {
                return false;
            }
        }
    }

private:
    RingBuffer& m_ring_buffer;
    const WsReaderSetting& m_setting;
    std::string& m_message;
    WsOpcode& m_opcode;
    ReadvAwaitableType m_readv_awaitable;
    bool m_is_server;
    SocketType& m_socket;
    bool m_use_mask;
    size_t m_total_received;
    bool m_first_frame;
    ControlFrameCallback m_control_frame_callback;

public:
    std::expected<bool, galay::kernel::IOError> m_result;
};

/**
 * @brief WebSocket读取器模板类
 */
template<typename SocketType>
class WsReaderImpl
{
public:
    WsReaderImpl(RingBuffer& ring_buffer, const WsReaderSetting& setting, SocketType& socket, bool is_server = true, bool use_mask = false)
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_socket(socket)
        , m_is_server(is_server)
        , m_use_mask(use_mask)
    {
    }

    GetFrameAwaitableImpl<SocketType> getFrame(WsFrame& frame) {
        return GetFrameAwaitableImpl<SocketType>(m_ring_buffer, m_setting, frame,
                                m_socket.readv(m_ring_buffer.getWriteIovecs()),
                                m_is_server);
    }

    GetMessageAwaitableImpl<SocketType> getMessage(std::string& message, WsOpcode& opcode) {
        return GetMessageAwaitableImpl<SocketType>(m_ring_buffer, m_setting, message, opcode,
                                  m_socket.readv(m_ring_buffer.getWriteIovecs()),
                                  m_is_server, m_socket, m_use_mask);
    }

private:
    RingBuffer& m_ring_buffer;
    const WsReaderSetting& m_setting;
    SocketType& m_socket;
    bool m_is_server;
    bool m_use_mask;
};

// 类型别名 - WebSocket over TCP
using GetFrameAwaitable = GetFrameAwaitableImpl<TcpSocket>;
using GetMessageAwaitable = GetMessageAwaitableImpl<TcpSocket>;
using WsReader = WsReaderImpl<TcpSocket>;

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-socket/async/SslSocket.h"
using GetFrameAwaitableSsl = GetFrameAwaitableImpl<galay::async::SslSocket>;
using GetMessageAwaitableSsl = GetMessageAwaitableImpl<galay::async::SslSocket>;
using WssReader = WsReaderImpl<galay::async::SslSocket>;
#endif

} // namespace galay::websocket

#endif // GALAY_WS_READER_H
