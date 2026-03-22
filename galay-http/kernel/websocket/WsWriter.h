#ifndef GALAY_WS_WRITER_H
#define GALAY_WS_WRITER_H

#include "WsWriterSetting.h"
#include "galay-http/kernel/IoVecUtils.h"
#include "galay-http/protoc/websocket/WebSocketFrame.h"
#include "galay-http/protoc/websocket/WebSocketError.h"
#include "galay-kernel/kernel/Awaitable.h"
#include "galay-kernel/async/TcpSocket.h"
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <sys/uio.h>

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/async/SslAwaitableCore.h"
#include "galay-ssl/async/SslSocket.h"
#endif

namespace galay::websocket
{

using namespace galay::kernel;
using namespace galay::async;

template<typename SocketType>
class WsWriterImpl;

template<typename T>
struct is_tcp_socket : std::false_type {};

template<>
struct is_tcp_socket<TcpSocket> : std::true_type {};

template<typename T>
inline constexpr bool is_tcp_socket_v = is_tcp_socket<T>::value;

template<typename T>
struct is_ws_writer_ssl_socket : std::false_type {};

#ifdef GALAY_HTTP_SSL_ENABLED
template<>
struct is_ws_writer_ssl_socket<galay::ssl::SslSocket> : std::true_type {};
#endif

template<typename T>
inline constexpr bool is_ws_writer_ssl_socket_v = is_ws_writer_ssl_socket<T>::value;

namespace detail {

template<typename SocketType>
struct WsTcpWritevMachine {
    using result_type = std::expected<bool, WsError>;
    static constexpr auto kSequenceOwnerDomain = galay::kernel::SequenceOwnerDomain::Write;

    explicit WsTcpWritevMachine(WsWriterImpl<SocketType>* writer)
        : m_writer(writer) {}

    MachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        if (m_writer->getRemainingBytes() == 0) {
            m_result = true;
            return MachineAction<result_type>::complete(true);
        }

        const auto* iov_data = m_writer->getIovecsData();
        const auto iov_count = m_writer->getIovecsCount();
        if (iov_data == nullptr || iov_count == 0) {
            failWithMessage("No remaining iovec to write");
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        return MachineAction<result_type>::waitWritev(iov_data, iov_count);
    }

    void onRead(std::expected<size_t, IOError>) {}

    void onWrite(std::expected<size_t, IOError> result) {
        if (!result) {
            m_result = std::unexpected(WsError(kWsSendError, result.error().message()));
            return;
        }

        const size_t written = result.value();
        if (written > 0) {
            m_writer->updateRemainingWritev(written);
        }

        if (m_writer->getRemainingBytes() == 0) {
            m_result = true;
            return;
        }

        if (m_writer->getIovecsData() == nullptr || m_writer->getIovecsCount() == 0) {
            failWithMessage("No remaining iovec to write");
        }
    }

private:
    void failWithMessage(const char* message) {
        m_result = std::unexpected(WsError(kWsSendError, message));
    }

    WsWriterImpl<SocketType>* m_writer;
    std::optional<result_type> m_result;
};

#ifdef GALAY_HTTP_SSL_ENABLED
template<typename SocketType>
struct WsSslSendMachine {
    using result_type = std::expected<bool, WsError>;
    static constexpr auto kSequenceOwnerDomain = galay::kernel::SequenceOwnerDomain::Write;

    explicit WsSslSendMachine(WsWriterImpl<SocketType>* writer)
        : m_writer(writer) {}

    galay::ssl::SslMachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        if (m_writer->getRemainingBytes() == 0) {
            m_result = true;
            return galay::ssl::SslMachineAction<result_type>::complete(true);
        }

        return galay::ssl::SslMachineAction<result_type>::send(
            m_writer->bufferData() + m_writer->sentBytes(),
            m_writer->getRemainingBytes());
    }

    void onHandshake(std::expected<void, galay::ssl::SslError>) {}
    void onRecv(std::expected<Bytes, galay::ssl::SslError>) {}
    void onShutdown(std::expected<void, galay::ssl::SslError>) {}

    void onSend(std::expected<size_t, galay::ssl::SslError> result) {
        if (!result) {
            m_writer->resetPendingState();
            m_result = std::unexpected(WsError(kWsSendError, result.error().message()));
            return;
        }

        if (result.value() == 0) {
            m_writer->resetPendingState();
            m_result = std::unexpected(WsError(kWsSendError, "SSL send returned zero bytes"));
            return;
        }

        m_writer->updateRemaining(result.value());
        if (m_writer->getRemainingBytes() == 0) {
            m_result = true;
        }
    }

private:
    WsWriterImpl<SocketType>* m_writer;
    std::optional<result_type> m_result;
};
#endif

template<typename SocketType>
auto buildSendAwaitable(SocketType& socket, WsWriterImpl<SocketType>& writer) {
    using ResultType = std::expected<bool, WsError>;
    if constexpr (is_ws_writer_ssl_socket_v<SocketType>) {
#ifdef GALAY_HTTP_SSL_ENABLED
        return galay::ssl::SslAwaitableBuilder<ResultType>::fromStateMachine(
                   socket.controller(),
                   &socket,
                   WsSslSendMachine<SocketType>(&writer))
            .build();
#else
        static_assert(!sizeof(SocketType), "SSL support is disabled");
#endif
    } else {
        return AwaitableBuilder<ResultType>::fromStateMachine(
                   socket.controller(),
                   WsTcpWritevMachine<SocketType>(&writer))
            .build();
    }
}

} // namespace detail

template<typename SocketType>
class WsWriterImpl
{
public:
    WsWriterImpl(const WsWriterSetting& setting, SocketType& socket)
        : m_setting(setting)
        , m_socket(&socket)
        , m_remaining_bytes(0)
    {
    }

    auto sendText(const std::string& text, bool fin = true) {
        if (m_remaining_bytes == 0) {
            WsFrame frame = WsFrameParser::createTextFrame(text, fin);
            prepareSendFrame(std::move(frame));
        }
        return makeSendAwaitable();
    }

    auto sendText(std::string&& text, bool fin = true) {
        if (m_remaining_bytes == 0) {
            WsFrame frame = WsFrameBuilder().text(std::move(text), fin).buildMove();
            prepareSendFrame(std::move(frame));
        }
        return makeSendAwaitable();
    }

    auto sendBinary(const std::string& data, bool fin = true) {
        if (m_remaining_bytes == 0) {
            WsFrame frame = WsFrameParser::createBinaryFrame(data, fin);
            prepareSendFrame(std::move(frame));
        }
        return makeSendAwaitable();
    }

    auto sendBinary(std::string&& data, bool fin = true) {
        if (m_remaining_bytes == 0) {
            WsFrame frame = WsFrameBuilder().binary(std::move(data), fin).buildMove();
            prepareSendFrame(std::move(frame));
        }
        return makeSendAwaitable();
    }

    auto sendPing(const std::string& data = "") {
        if (m_remaining_bytes == 0) {
            WsFrame frame = WsFrameParser::createPingFrame(data);
            prepareSendFrame(std::move(frame));
        }
        return makeSendAwaitable();
    }

    auto sendPong(const std::string& data = "") {
        if (m_remaining_bytes == 0) {
            WsFrame frame = WsFrameParser::createPongFrame(data);
            prepareSendFrame(std::move(frame));
        }
        return makeSendAwaitable();
    }

    auto sendClose(WsCloseCode code = WsCloseCode::Normal, const std::string& reason = "") {
        if (m_remaining_bytes == 0) {
            WsFrame frame = WsFrameParser::createCloseFrame(code, reason);
            prepareSendFrame(std::move(frame));
        }
        return makeSendAwaitable();
    }

    auto sendFrame(const WsFrame& frame) {
        if (m_remaining_bytes == 0) {
            prepareSendFrame(frame);
        }
        return makeSendAwaitable();
    }

    auto sendFrame(WsFrame&& frame) {
        if (m_remaining_bytes == 0) {
            prepareSendFrame(std::move(frame));
        }
        return makeSendAwaitable();
    }

    void prepareSslMessage(WsOpcode opcode, std::string_view payload, bool fin = true) {
        resetPendingState();
        WsFrame frame = WsFrameBuilder()
            .opcode(opcode)
            .fin(fin)
            .payload(std::string(payload))
            .buildMove();
        prepareSendFrame(std::move(frame));
    }

private:
    auto makeSendAwaitable() {
        return detail::buildSendAwaitable(*m_socket, *this);
    }

    void prepareSendFrame(const WsFrame& frame) {
        if constexpr (is_tcp_socket_v<SocketType>) {
            prepareWritevBuffers(frame);
        } else {
            WsFrameParser::encodeInto(m_buffer, frame, m_setting.use_mask);
            m_remaining_bytes = m_buffer.size();
        }
    }

    void prepareSendFrame(WsFrame&& frame) {
        if constexpr (is_tcp_socket_v<SocketType>) {
            prepareWritevBuffers(std::move(frame));
        } else {
            WsFrameParser::encodeInto(m_buffer, frame, m_setting.use_mask);
            m_remaining_bytes = m_buffer.size();
        }
    }

    void prepareWritevBuffers(const WsFrame& frame) {
        m_buffer = WsFrameParser::toBytesHeader(frame, m_setting.use_mask, m_masking_key);
        m_payload_buffer = frame.payload;
        finalizeWritevBuffers();
    }

    void prepareWritevBuffers(WsFrame&& frame) {
        m_buffer = WsFrameParser::toBytesHeader(frame, m_setting.use_mask, m_masking_key);
        m_payload_buffer = std::move(frame.payload);
        finalizeWritevBuffers();
    }

    void finalizeWritevBuffers() {
        if (m_setting.use_mask && !m_payload_buffer.empty()) {
            WsFrameParser::applyMask(m_payload_buffer, m_masking_key);
        }

        std::vector<iovec> iovecs;
        iovecs.reserve(2);
        iovecs.push_back({const_cast<char*>(m_buffer.data()), m_buffer.size()});
        if (!m_payload_buffer.empty()) {
            iovecs.push_back({const_cast<char*>(m_payload_buffer.data()), m_payload_buffer.size()});
        }

        m_writev_cursor.reset(std::move(iovecs));
        m_remaining_bytes = m_writev_cursor.remainingBytes();
    }

public:
    void resetPendingState() {
        m_buffer.clear();
        m_payload_buffer.clear();
        m_writev_cursor.reset(std::vector<iovec>{});
        m_remaining_bytes = 0;
    }

    void updateRemaining(size_t bytes_sent) {
        if (bytes_sent >= m_remaining_bytes) {
            m_remaining_bytes = 0;
            m_buffer.clear();
        } else {
            m_remaining_bytes -= bytes_sent;
        }
    }

    void updateRemainingWritev(size_t bytes_sent) {
        const size_t advanced = m_writev_cursor.advance(bytes_sent);
        if (advanced >= m_remaining_bytes) {
            m_remaining_bytes = 0;
            m_buffer.clear();
            m_payload_buffer.clear();
            m_writev_cursor.reset(std::vector<iovec>{});
            return;
        }

        m_remaining_bytes -= advanced;
    }

    size_t getRemainingBytes() const {
        return m_remaining_bytes;
    }

    const char* bufferData() const {
        return m_buffer.data();
    }

    size_t sentBytes() const {
        return m_buffer.size() - m_remaining_bytes;
    }

    const iovec* getIovecsData() const {
        return m_writev_cursor.data();
    }

    size_t getIovecsCount() const {
        return m_writev_cursor.count();
    }

private:
    WsWriterSetting m_setting;
    SocketType* m_socket;
    std::string m_buffer;
    std::string m_payload_buffer;
    IoVecCursor m_writev_cursor;
    size_t m_remaining_bytes;
    uint8_t m_masking_key[4];
};

using WsWriter = WsWriterImpl<TcpSocket>;

} // namespace galay::websocket

#ifdef GALAY_HTTP_SSL_ENABLED
namespace galay::websocket {
using WssWriter = WsWriterImpl<galay::ssl::SslSocket>;
} // namespace galay::websocket
#endif

#endif // GALAY_WS_WRITER_H
