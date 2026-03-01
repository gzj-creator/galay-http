#ifndef GALAY_WS_WRITER_H
#define GALAY_WS_WRITER_H

#include "WsWriterSetting.h"
#include "galay-http/protoc/websocket/WebSocketFrame.h"
#include "galay-http/protoc/websocket/WebSocketError.h"
#include "galay-kernel/kernel/Awaitable.h"
#include "galay-kernel/kernel/Timeout.hpp"
#include "galay-kernel/async/TcpSocket.h"
#include <expected>
#include <coroutine>
#include <optional>
#include <string>
#include <cstring>
#include <utility>
#include <sys/uio.h>

namespace galay::websocket
{

using namespace galay::kernel;
using namespace galay::async;

// 前向声明
template<typename SocketType>
class WsWriterImpl;

// 类型萃取：判断是否为 TcpSocket
template<typename T>
struct is_tcp_socket : std::false_type {};

template<>
struct is_tcp_socket<TcpSocket> : std::true_type {};

template<typename T>
inline constexpr bool is_tcp_socket_v = is_tcp_socket<T>::value;

template<typename SocketType, bool IsTcp = is_tcp_socket_v<SocketType>>
class SendFrameAwaitableImpl;

/**
 * @brief WebSocket帧发送等待体（TcpSocket: CustomAwaitable + WritevAwaitable）
 */
template<typename SocketType>
class SendFrameAwaitableImpl<SocketType, true>
    : public galay::kernel::CustomAwaitable
    , public galay::kernel::TimeoutSupport<SendFrameAwaitableImpl<SocketType, true>>
{
public:
    class ProtocolWritevAwaitable : public galay::kernel::WritevAwaitable
    {
    public:
        explicit ProtocolWritevAwaitable(SendFrameAwaitableImpl* owner)
            : galay::kernel::WritevAwaitable(owner->m_socket->controller(), std::vector<iovec>{})
            , m_owner(owner)
        {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle) override {
            if (m_owner->m_writer->getRemainingBytes() == 0) {
                return true;
            }

            if (cqe == nullptr) {
                syncIovecs();
                if (m_iovecs.empty()) {
                    m_owner->setSendError(WsError(kWsSendError, "No data to write"));
                    return true;
                }
                return false;
            }

            auto result = galay::kernel::io::handleWritev(cqe);
            if (!result && galay::kernel::IOError::contains(result.error().code(), galay::kernel::kNotReady)) {
                return false;
            }
            if (!result) {
                m_owner->setSendError(result.error());
                return true;
            }

            size_t bytes_written = result.value();
            if (bytes_written == 0) {
                return false;
            }

            m_owner->m_writer->updateRemainingWritev(bytes_written);
            if (m_owner->m_writer->getRemainingBytes() == 0) {
                return true;
            }

            syncIovecs();
            if (m_iovecs.empty()) {
                m_owner->setSendError(WsError(kWsSendError, "No remaining iovec to write"));
                return true;
            }
            return false;
        }
#else
        bool handleComplete(GHandle handle) override {
            while (m_owner->m_writer->getRemainingBytes() > 0) {
                const auto& iovecs = m_owner->m_writer->getIovecsRef();
                if (iovecs.empty()) {
                    m_owner->setSendError(WsError(kWsSendError, "No remaining iovec to write"));
                    return true;
                }

                auto result = galay::kernel::io::handleWritev(handle,
                                                              const_cast<iovec*>(iovecs.data()),
                                                              static_cast<int>(iovecs.size()));
                if (!result && galay::kernel::IOError::contains(result.error().code(), galay::kernel::kNotReady)) {
                    return false;
                }
                if (!result) {
                    m_owner->setSendError(result.error());
                    return true;
                }

                size_t bytes_written = result.value();
                if (bytes_written == 0) {
                    return false;
                }

                m_owner->m_writer->updateRemainingWritev(bytes_written);
            }

            return true;
        }
#endif

    private:
#ifdef USE_IOURING
        void syncIovecs() {
            const auto& src = m_owner->m_writer->getIovecsRef();
            m_iovecs.resize(src.size());
            if (!src.empty()) {
                std::memcpy(m_iovecs.data(), src.data(), src.size() * sizeof(iovec));
            }
        }
#endif

        SendFrameAwaitableImpl* m_owner;
    };

    SendFrameAwaitableImpl(WsWriterImpl<SocketType>& writer, SocketType& socket)
        : galay::kernel::CustomAwaitable(socket.controller())
        , m_writer(&writer)
        , m_socket(&socket)
        , m_writev_awaitable(this)
        , m_result(0)
    {
        addTask(IOEventType::WRITEV, &m_writev_awaitable);
    }

    bool await_ready() const noexcept {
        return false;
    }

    using galay::kernel::CustomAwaitable::await_suspend;

    std::expected<bool, WsError> await_resume() {
        onCompleted();

        if (!m_result.has_value()) {
            return std::unexpected(WsError(kWsSendError, m_result.error().message()));
        }

        if (m_ws_error.has_value()) {
            return std::unexpected(std::move(*m_ws_error));
        }

        return true;
    }

private:
    void setSendError(const galay::kernel::IOError& io_error) {
        m_ws_error = WsError(kWsSendError, io_error.message());
    }

    void setSendError(WsError&& ws_error) {
        m_ws_error = std::move(ws_error);
    }

    WsWriterImpl<SocketType>* m_writer;
    SocketType* m_socket;
    ProtocolWritevAwaitable m_writev_awaitable;
    std::optional<WsError> m_ws_error;

public:
    std::expected<size_t, galay::kernel::IOError> m_result;
};

/**
 * @brief WebSocket帧发送等待体（非 TcpSocket: send 版本）
 */
template<typename SocketType>
class SendFrameAwaitableImpl<SocketType, false>
    : public galay::kernel::TimeoutSupport<SendFrameAwaitableImpl<SocketType, false>>
{
public:
    using SendAwaitableType = decltype(std::declval<SocketType>().send(std::declval<const char*>(), std::declval<size_t>()));

    SendFrameAwaitableImpl(WsWriterImpl<SocketType>& writer, SendAwaitableType&& send_awaitable)
        : m_writer(writer)
        , m_send_awaitable(std::move(send_awaitable))
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    auto await_suspend(std::coroutine_handle<> handle) {
        return m_send_awaitable.await_suspend(handle);
    }

    std::expected<bool, WsError> await_resume() {
        auto send_result = m_send_awaitable.await_resume();
        if (!send_result) {
            return std::unexpected(WsError(kWsSendError, send_result.error().message()));
        }

        size_t bytes_written = send_result.value();
        m_writer.updateRemaining(bytes_written);

        if (m_writer.getRemainingBytes() == 0) {
            return true;
        }
        return false;
    }

private:
    WsWriterImpl<SocketType>& m_writer;
    SendAwaitableType m_send_awaitable;

public:
    std::expected<size_t, galay::kernel::IOError> m_result;
};

/**
 * @brief WebSocket写入器模板类
 */
template<typename SocketType>
class WsWriterImpl
{
public:
    WsWriterImpl(const WsWriterSetting& setting, SocketType& socket)
        : m_setting(setting)
        , m_socket(&socket)
        , m_remaining_bytes(0)
        , m_writev_offset(0)
    {
        m_iovecs.reserve(2);
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
            WsFrame frame(WsOpcode::Text, std::move(text), fin);
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
            WsFrame frame(WsOpcode::Binary, std::move(data), fin);
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

private:
    SendFrameAwaitableImpl<SocketType> makeSendAwaitable() {
        if constexpr (is_tcp_socket_v<SocketType>) {
            return SendFrameAwaitableImpl<SocketType>(*this, *m_socket);
        } else {
            const size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
            const char* send_ptr = m_buffer.data() + sent_bytes;
            return SendFrameAwaitableImpl<SocketType>(*this, m_socket->send(send_ptr, m_remaining_bytes));
        }
    }

    void prepareSendFrame(const WsFrame& frame) {
        if constexpr (is_tcp_socket_v<SocketType>) {
            prepareWritevBuffers(frame);
        } else {
            m_buffer = WsFrameParser::toBytes(frame, m_setting.use_mask);
            m_remaining_bytes = m_buffer.size();
        }
    }

    void prepareSendFrame(WsFrame&& frame) {
        if constexpr (is_tcp_socket_v<SocketType>) {
            prepareWritevBuffers(std::move(frame));
        } else {
            m_buffer = WsFrameParser::toBytes(frame, m_setting.use_mask);
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

        m_iovecs.clear();
        m_iovecs.push_back({const_cast<char*>(m_buffer.data()), m_buffer.size()});
        if (!m_payload_buffer.empty()) {
            m_iovecs.push_back({const_cast<char*>(m_payload_buffer.data()), m_payload_buffer.size()});
        }

        m_remaining_bytes = m_buffer.size() + m_payload_buffer.size();
        m_writev_offset = 0;
    }

public:

    void updateRemaining(size_t bytes_sent) {
        if (bytes_sent >= m_remaining_bytes) {
            m_remaining_bytes = 0;
            m_buffer.clear();
        } else {
            m_remaining_bytes -= bytes_sent;
        }
    }

    void updateRemainingWritev(size_t bytes_sent) {
        if (bytes_sent >= m_remaining_bytes) {
            m_remaining_bytes = 0;
            m_buffer.clear();
            m_payload_buffer.clear();
            m_iovecs.clear();
            m_writev_offset = 0;
            return;
        }

        m_remaining_bytes -= bytes_sent;
        m_writev_offset += bytes_sent;

        // 更新 iovec 数组，跳过已发送的部分
        size_t offset = m_writev_offset;
        m_iovecs.clear();

        // 检查 header 是否已完全发送
        if (offset < m_buffer.size()) {
            // header 还有剩余
            m_iovecs.push_back({const_cast<char*>(m_buffer.data()) + offset, m_buffer.size() - offset});
            if (!m_payload_buffer.empty()) {
                m_iovecs.push_back({const_cast<char*>(m_payload_buffer.data()), m_payload_buffer.size()});
            }
        } else {
            // header 已完全发送，只剩 payload
            size_t payload_offset = offset - m_buffer.size();
            if (payload_offset < m_payload_buffer.size()) {
                m_iovecs.push_back({const_cast<char*>(m_payload_buffer.data()) + payload_offset, m_payload_buffer.size() - payload_offset});
            }
        }
    }

    size_t getRemainingBytes() const {
        return m_remaining_bytes;
    }

    const std::vector<iovec>& getIovecsRef() const {
        return m_iovecs;
    }

private:
    WsWriterSetting m_setting;
    SocketType* m_socket;
    std::string m_buffer;           // 存储 header
    std::string m_payload_buffer;   // 存储 payload（用于 writev）
    std::vector<iovec> m_iovecs;    // iovec 数组（用于 writev）
    size_t m_remaining_bytes;
    size_t m_writev_offset;         // writev 偏移量
    uint8_t m_masking_key[4];       // 掩码密钥
};

// 类型别名 - WebSocket over TCP
using SendFrameAwaitable = SendFrameAwaitableImpl<TcpSocket>;
using WsWriter = WsWriterImpl<TcpSocket>;

} // namespace galay::websocket

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/async/SslSocket.h"
namespace galay::websocket {
using SendFrameAwaitableSsl = SendFrameAwaitableImpl<galay::ssl::SslSocket>;
using WssWriter = WsWriterImpl<galay::ssl::SslSocket>;
} // namespace galay::websocket
#endif

#endif // GALAY_WS_WRITER_H
