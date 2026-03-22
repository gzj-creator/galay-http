#ifndef GALAY_WS_READER_H
#define GALAY_WS_READER_H

#include "WsReaderSetting.h"
#include "galay-http/kernel/IoVecUtils.h"
#include "galay-http/protoc/websocket/WebSocketError.h"
#include "galay-http/protoc/websocket/WebSocketFrame.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/common/Buffer.h"
#include "galay-kernel/kernel/Awaitable.h"
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/async/SslAwaitableCore.h"
#include "galay-ssl/async/SslSocket.h"
#endif

namespace galay::websocket {

using namespace galay::async;
using namespace galay::kernel;

template<typename T>
struct is_ssl_socket : std::false_type {};

#ifdef GALAY_HTTP_SSL_ENABLED
template<>
struct is_ssl_socket<galay::ssl::SslSocket> : std::true_type {};
#endif

template<typename T>
inline constexpr bool is_ssl_socket_v = is_ssl_socket<T>::value;

using ControlFrameCallback = std::function<void(WsOpcode opcode, const std::string& payload)>;

namespace detail {

template<typename StateT>
struct WsRingBufferTcpReadMachine {
    using result_type = typename StateT::ResultType;
    static constexpr auto kSequenceOwnerDomain = galay::kernel::SequenceOwnerDomain::Read;

    explicit WsRingBufferTcpReadMachine(std::shared_ptr<StateT> state)
        : m_state(std::move(state)) {}

    MachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        if (m_state->parseFromBuffer()) {
            m_result = m_state->takeResult();
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        if (!m_state->prepareRecvWindow()) {
            m_result = m_state->takeResult();
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        return MachineAction<result_type>::waitReadv(
            m_state->recvIovecsData(),
            m_state->recvIovecsCount());
    }

    void onRead(std::expected<size_t, IOError> result) {
        if (!result) {
            m_state->setRecvError(result.error());
            m_result = m_state->takeResult();
            return;
        }

        if (result.value() == 0) {
            m_state->onPeerClosed();
            m_result = m_state->takeResult();
            return;
        }

        m_state->onBytesReceived(result.value());
    }

    void onWrite(std::expected<size_t, IOError>) {}

    std::shared_ptr<StateT> m_state;
    std::optional<result_type> m_result;
};

#ifdef GALAY_HTTP_SSL_ENABLED
template<typename StateT>
struct WsRingBufferSslReadMachine {
    using result_type = typename StateT::ResultType;
    static constexpr auto kSequenceOwnerDomain = galay::kernel::SequenceOwnerDomain::Read;

    explicit WsRingBufferSslReadMachine(std::shared_ptr<StateT> state)
        : m_state(std::move(state)) {}

    galay::ssl::SslMachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        if (m_state->parseFromBuffer()) {
            m_result = m_state->takeResult();
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        char* recv_buffer = nullptr;
        size_t recv_length = 0;
        if (!m_state->prepareRecvWindow(recv_buffer, recv_length)) {
            m_result = m_state->takeResult();
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        return galay::ssl::SslMachineAction<result_type>::recv(recv_buffer, recv_length);
    }

    void onHandshake(std::expected<void, galay::ssl::SslError>) {}

    void onRecv(std::expected<Bytes, galay::ssl::SslError> result) {
        if (!result) {
            m_state->setSslRecvError(result.error());
            m_result = m_state->takeResult();
            return;
        }

        const size_t recv_bytes = result.value().size();
        if (recv_bytes == 0) {
            m_state->onPeerClosed();
            m_result = m_state->takeResult();
            return;
        }

        m_state->onBytesReceived(recv_bytes);
    }

    void onSend(std::expected<size_t, galay::ssl::SslError>) {}

    void onShutdown(std::expected<void, galay::ssl::SslError>) {}

    std::shared_ptr<StateT> m_state;
    std::optional<result_type> m_result;
};
#endif

struct WsFrameReadState {
    using ResultType = std::expected<bool, WsError>;

    WsFrameReadState(RingBuffer& ring_buffer,
                     const WsReaderSetting& setting,
                     WsFrame& frame,
                     bool is_server)
        : m_ring_buffer(&ring_buffer)
        , m_setting(&setting)
        , m_frame(&frame)
        , m_is_server(is_server) {}

    bool parseFromBuffer() {
        auto read_iovecs = borrowReadIovecs(*m_ring_buffer);
        if (read_iovecs.empty()) {
            return false;
        }

        auto parse_result = WsFrameParser::fromIOVec(
            read_iovecs.data(),
            read_iovecs.size(),
            *m_frame,
            m_is_server);
        if (!parse_result.has_value()) {
            WsError error = parse_result.error();
            if (error.code() == kWsIncomplete) {
                const size_t buffered = m_ring_buffer->readable();
                if (m_total_received + buffered > m_setting->max_frame_size) {
                    setParseError(WsError(kWsMessageTooLarge, "Frame size exceeds limit"));
                    return true;
                }
                return false;
            }
            setParseError(std::move(error));
            return true;
        }

        m_ring_buffer->consume(parse_result.value());
        if (m_frame->header.payload_length > m_setting->max_frame_size) {
            setParseError(WsError(kWsMessageTooLarge, "Frame payload too large"));
            return true;
        }
        return true;
    }

    bool prepareRecvWindow() {
        m_write_iovecs = borrowWriteIovecs(*m_ring_buffer);
        if (m_write_iovecs.empty()) {
            setParseError(WsError(kWsConnectionError, "Ring buffer has no space for writing"));
            return false;
        }
        return true;
    }

    bool prepareRecvWindow(char*& buffer, size_t& length) {
        if (!prepareRecvWindow()) {
            buffer = nullptr;
            length = 0;
            return false;
        }
        if (!IoVecWindow::bindFirstNonEmpty(m_write_iovecs, buffer, length)) {
            setParseError(WsError(kWsConnectionError, "Ring buffer has no space for writing"));
            return false;
        }
        return true;
    }

    const struct iovec* recvIovecsData() const { return m_write_iovecs.data(); }
    size_t recvIovecsCount() const { return m_write_iovecs.size(); }

    void setRecvError(const IOError& io_error) {
        if (IOError::contains(io_error.code(), kDisconnectError)) {
            m_ws_error = WsError(kWsConnectionClosed, io_error.message());
            return;
        }
        m_ws_error = WsError(kWsConnectionError, io_error.message());
    }

#ifdef GALAY_HTTP_SSL_ENABLED
    void setSslRecvError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            m_ws_error = WsError(kWsConnectionClosed, "Connection closed by peer");
            return;
        }
        m_ws_error = WsError(kWsConnectionError, error.message());
    }
#endif

    void onPeerClosed() {
        m_ws_error = WsError(kWsConnectionClosed, "Connection closed by peer");
    }

    void onBytesReceived(size_t recv_bytes) {
        m_total_received += recv_bytes;
        m_ring_buffer->produce(recv_bytes);
    }

    void setParseError(WsError&& error) { m_ws_error = std::move(error); }

    ResultType takeResult() {
        if (m_ws_error.has_value()) {
            return std::unexpected(std::move(*m_ws_error));
        }
        return true;
    }

    RingBuffer* m_ring_buffer;
    const WsReaderSetting* m_setting;
    WsFrame* m_frame;
    bool m_is_server;
    size_t m_total_received = 0;
    BorrowedIovecs<2> m_write_iovecs;
    std::optional<WsError> m_ws_error;
};

struct WsMessageReadState {
    using ResultType = std::expected<bool, WsError>;

    WsMessageReadState(RingBuffer& ring_buffer,
                       const WsReaderSetting& setting,
                       std::string& message,
                       WsOpcode& opcode,
                       bool is_server,
                       bool use_mask,
                       ControlFrameCallback control_frame_callback)
        : m_ring_buffer(&ring_buffer)
        , m_setting(&setting)
        , m_message(&message)
        , m_opcode(&opcode)
        , m_is_server(is_server)
        , m_use_mask(use_mask)
        , m_control_frame_callback(std::move(control_frame_callback)) {}

    bool parseFromBuffer() {
        while (true) {
            auto read_iovecs = borrowReadIovecs(*m_ring_buffer);
            if (read_iovecs.empty()) {
                return false;
            }

            WsFrame frame;
            auto parse_result = WsFrameParser::fromIOVec(
                read_iovecs.data(),
                read_iovecs.size(),
                frame,
                m_is_server);
            if (!parse_result.has_value()) {
                WsError error = parse_result.error();
                if (error.code() == kWsIncomplete) {
                    const size_t buffered = m_ring_buffer->readable();
                    if (m_message->size() + m_total_received + buffered > m_setting->max_message_size) {
                        setParseError(WsError(kWsMessageTooLarge, "Message size exceeds limit"));
                        return true;
                    }
                    return false;
                }
                setParseError(std::move(error));
                return true;
            }

            m_ring_buffer->consume(parse_result.value());

            if (isControlFrame(frame.header.opcode)) {
                if (!frame.header.fin) {
                    setParseError(WsError(kWsControlFrameFragmented));
                    return true;
                }
                if (m_control_frame_callback) {
                    m_control_frame_callback(frame.header.opcode, frame.payload);
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

            if (m_ring_buffer->readable() == 0) {
                return false;
            }
        }
    }

    bool prepareRecvWindow() {
        m_write_iovecs = borrowWriteIovecs(*m_ring_buffer);
        if (m_write_iovecs.empty()) {
            setParseError(WsError(kWsConnectionError, "Ring buffer has no space for writing"));
            return false;
        }
        return true;
    }

    bool prepareRecvWindow(char*& buffer, size_t& length) {
        if (!prepareRecvWindow()) {
            buffer = nullptr;
            length = 0;
            return false;
        }
        if (!IoVecWindow::bindFirstNonEmpty(m_write_iovecs, buffer, length)) {
            setParseError(WsError(kWsConnectionError, "Ring buffer has no space for writing"));
            return false;
        }
        return true;
    }

    const struct iovec* recvIovecsData() const { return m_write_iovecs.data(); }
    size_t recvIovecsCount() const { return m_write_iovecs.size(); }

    void setRecvError(const IOError& io_error) {
        if (IOError::contains(io_error.code(), kDisconnectError)) {
            m_ws_error = WsError(kWsConnectionClosed, io_error.message());
            return;
        }
        m_ws_error = WsError(kWsConnectionError, io_error.message());
    }

#ifdef GALAY_HTTP_SSL_ENABLED
    void setSslRecvError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            m_ws_error = WsError(kWsConnectionClosed, "Connection closed by peer");
            return;
        }
        m_ws_error = WsError(kWsConnectionError, error.message());
    }
#endif

    void onPeerClosed() {
        m_ws_error = WsError(kWsConnectionClosed, "Connection closed by peer");
    }

    void onBytesReceived(size_t recv_bytes) {
        m_total_received += recv_bytes;
        m_ring_buffer->produce(recv_bytes);
    }

    void setParseError(WsError&& error) { m_ws_error = std::move(error); }

    ResultType takeResult() {
        if (m_ws_error.has_value()) {
            return std::unexpected(std::move(*m_ws_error));
        }
        return true;
    }

    RingBuffer* m_ring_buffer;
    const WsReaderSetting* m_setting;
    std::string* m_message;
    WsOpcode* m_opcode;
    bool m_is_server;
    bool m_use_mask;
    size_t m_total_received = 0;
    bool m_first_frame = true;
    ControlFrameCallback m_control_frame_callback;
    BorrowedIovecs<2> m_write_iovecs;
    std::optional<WsError> m_ws_error;
};

template<typename SocketType, typename StateT>
auto buildReadOperation(SocketType& socket, std::shared_ptr<StateT> state) {
    using ResultType = typename StateT::ResultType;
    if constexpr (is_ssl_socket_v<SocketType>) {
#ifdef GALAY_HTTP_SSL_ENABLED
        return galay::ssl::SslAwaitableBuilder<ResultType>::fromStateMachine(
                   socket.controller(),
                   &socket,
                   WsRingBufferSslReadMachine<StateT>(std::move(state)))
            .build();
#else
        static_assert(!sizeof(SocketType), "SSL support is disabled");
#endif
    } else {
        return AwaitableBuilder<ResultType>::fromStateMachine(
                   socket.controller(),
                   WsRingBufferTcpReadMachine<StateT>(std::move(state)))
            .build();
    }
}

} // namespace detail

template<typename SocketType>
class WsReaderImpl {
public:
    WsReaderImpl(RingBuffer& ring_buffer,
                 const WsReaderSetting& setting,
                 SocketType& socket,
                 bool is_server = true,
                 bool use_mask = false)
        : m_ring_buffer(&ring_buffer)
        , m_setting(setting)
        , m_socket(&socket)
        , m_is_server(is_server)
        , m_use_mask(use_mask) {}

    auto getFrame(WsFrame& frame) {
        return detail::buildReadOperation(
            *m_socket,
            std::make_shared<detail::WsFrameReadState>(*m_ring_buffer, m_setting, frame, m_is_server));
    }

    auto getMessage(std::string& message, WsOpcode& opcode) {
        return detail::buildReadOperation(
            *m_socket,
            std::make_shared<detail::WsMessageReadState>(
                *m_ring_buffer,
                m_setting,
                message,
                opcode,
                m_is_server,
                m_use_mask,
                nullptr));
    }

private:
    RingBuffer* m_ring_buffer;
    WsReaderSetting m_setting;
    SocketType* m_socket;
    bool m_is_server;
    bool m_use_mask;
};

using WsReader = WsReaderImpl<TcpSocket>;

} // namespace galay::websocket

#ifdef GALAY_HTTP_SSL_ENABLED
namespace galay::websocket {
using WssReader = WsReaderImpl<galay::ssl::SslSocket>;
} // namespace galay::websocket
#endif

#endif // GALAY_WS_READER_H
