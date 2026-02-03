#ifndef GALAY_WS_SESSION_H
#define GALAY_WS_SESSION_H

#include "WsReader.h"
#include "WsWriter.h"
#include "WsUpgrade.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/kernel/http/HttpLog.h"
#include "galay-http/utils/Http1_1RequestBuilder.h"
#include "galay-kernel/common/Buffer.h"
#include "galay-kernel/async/TcpSocket.h"
#include <galay-utils/algorithm/Base64.hpp>
#include <string>
#include <optional>
#include <coroutine>

namespace galay::websocket
{

using namespace galay::async;
using namespace galay::kernel;
using namespace galay::http;

// 前向声明
struct WsUrl;
std::string generateWebSocketKey();

template<typename SocketType>
class WsSessionImpl;

template<typename SocketType>
class WsSessionUpgraderImpl;

template<typename SocketType>
class WsSessionUpgradeAwaitableImpl;

/**
 * @brief WebSocket Session 升级器
 * @details 管理升级过程中的临时变量和状态
 */
template<typename SocketType>
class WsSessionUpgraderImpl
{
public:
    using SendAwaitableType = decltype(std::declval<SocketType>().send(std::declval<const char*>(), std::declval<size_t>()));
    using ReadvAwaitableType = decltype(std::declval<SocketType>().readv(std::declval<std::vector<iovec>>()));

    WsSessionUpgraderImpl(WsSessionImpl<SocketType>* session)
        : m_session(session)
        , m_state(State::Invalid)
        , m_send_offset(0)
    {
    }

    /**
     * @brief 返回升级等待体
     * @return 可以 co_await 的等待体对象
     */
    WsSessionUpgradeAwaitableImpl<SocketType> operator()() {
        return WsSessionUpgradeAwaitableImpl<SocketType>(this);
    }

    friend class WsSessionUpgradeAwaitableImpl<SocketType>;

private:
    enum class State {
        Invalid,
        Sending,
        Receiving
    };

    WsSessionImpl<SocketType>* m_session;
    State m_state;

    std::optional<SendAwaitableType> m_send_awaitable;
    std::optional<ReadvAwaitableType> m_recv_awaitable;

    std::string m_send_buffer;
    size_t m_send_offset;
    std::string m_ws_key;
    HttpResponse m_upgrade_response;
};

/**
 * @brief WebSocket Session 升级等待体
 */
template<typename SocketType>
class WsSessionUpgradeAwaitableImpl
{
public:
    WsSessionUpgradeAwaitableImpl(WsSessionUpgraderImpl<SocketType>* upgrader)
        : m_upgrader(upgrader)
    {
    }

    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> handle);
    std::expected<bool, WsError> await_resume();

private:
    WsSessionUpgraderImpl<SocketType>* m_upgrader;
};

/**
 * @brief WebSocket会话模板类
 * @details 持有 socket、ring_buffer、reader 和 writer，负责WebSocket升级和通信
 */
template<typename SocketType>
class WsSessionImpl
{
public:
    using SendAwaitableType = decltype(std::declval<SocketType>().send(std::declval<const char*>(), std::declval<size_t>()));
    using ReadvAwaitableType = decltype(std::declval<SocketType>().readv(std::declval<std::vector<iovec>>()));

    WsSessionImpl(SocketType& socket,
                  const WsUrl& url,
                  size_t ring_buffer_size = 8192,
                  const WsReaderSetting& reader_setting = WsReaderSetting(),
                  const WsWriterSetting& writer_setting = WsWriterSetting())
        : m_socket(socket)
        , m_url(url)
        , m_ring_buffer(ring_buffer_size)
        , m_reader(m_ring_buffer, reader_setting, socket, false, true)  // is_server=false, use_mask=true (客户端)
        , m_writer(writer_setting, socket)
        , m_upgraded(false)
    {
    }

    WsReaderImpl<SocketType>& getReader() {
        return m_reader;
    }

    WsWriterImpl<SocketType>& getWriter() {
        return m_writer;
    }

    /**
     * @brief 执行WebSocket升级握手
     * @return 升级器对象
     */
    WsSessionUpgraderImpl<SocketType> upgrade() {
        return WsSessionUpgraderImpl<SocketType>(this);
    }

    bool isUpgraded() const {
        return m_upgraded;
    }

    // 便捷方法：发送文本消息
    SendFrameAwaitableImpl<SocketType> sendText(const std::string& text, bool fin = true) {
        return m_writer.sendText(text, fin);
    }

    // 便捷方法：发送二进制消息
    SendFrameAwaitableImpl<SocketType> sendBinary(const std::string& data, bool fin = true) {
        return m_writer.sendBinary(data, fin);
    }

    // 便捷方法：发送Ping
    SendFrameAwaitableImpl<SocketType> sendPing(const std::string& data = "") {
        return m_writer.sendPing(data);
    }

    // 便捷方法：发送Pong
    SendFrameAwaitableImpl<SocketType> sendPong(const std::string& data = "") {
        return m_writer.sendPong(data);
    }

    // 便捷方法：发送Close
    SendFrameAwaitableImpl<SocketType> sendClose(WsCloseCode code = WsCloseCode::Normal, const std::string& reason = "") {
        return m_writer.sendClose(code, reason);
    }

    // 便捷方法：接收消息
    GetMessageAwaitableImpl<SocketType> getMessage(std::string& message, WsOpcode& opcode) {
        return m_reader.getMessage(message, opcode);
    }

    // 便捷方法：接收帧
    GetFrameAwaitableImpl<SocketType> getFrame(WsFrame& frame) {
        return m_reader.getFrame(frame);
    }

    friend class WsSessionUpgradeAwaitableImpl<SocketType>;
    friend class WsSessionUpgraderImpl<SocketType>;

private:
    SocketType& m_socket;
    const WsUrl& m_url;
    RingBuffer m_ring_buffer;
    WsReaderImpl<SocketType> m_reader;
    WsWriterImpl<SocketType> m_writer;
    bool m_upgraded;
};

// WsSessionUpgradeAwaitableImpl 的实现
template<typename SocketType>
bool WsSessionUpgradeAwaitableImpl<SocketType>::await_suspend(std::coroutine_handle<> handle) {
    auto& upgrader = *m_upgrader;
    auto& session = *upgrader.m_session;

    if (upgrader.m_state == WsSessionUpgraderImpl<SocketType>::State::Invalid) {
        upgrader.m_state = WsSessionUpgraderImpl<SocketType>::State::Sending;

        // 生成WebSocket Key
        upgrader.m_ws_key = generateWebSocketKey();

        // 构建升级请求
        auto request = Http1_1RequestBuilder::get(session.m_url.path)
            .header("Host", session.m_url.host + ":" + std::to_string(session.m_url.port))
            .header("Upgrade", "websocket")
            .header("Connection", "Upgrade")
            .header("Sec-WebSocket-Key", upgrader.m_ws_key)
            .header("Sec-WebSocket-Version", "13")
            .build();

        upgrader.m_send_buffer = request.toString();
        upgrader.m_send_offset = 0;

        HTTP_LOG_INFO("Sending WebSocket upgrade request...");

        size_t remaining = upgrader.m_send_buffer.size() - upgrader.m_send_offset;
        const char* send_ptr = upgrader.m_send_buffer.data() + upgrader.m_send_offset;
        upgrader.m_send_awaitable.emplace(session.m_socket.send(send_ptr, remaining));
        return upgrader.m_send_awaitable->await_suspend(handle);

    } else if (upgrader.m_state == WsSessionUpgraderImpl<SocketType>::State::Sending) {
        size_t remaining = upgrader.m_send_buffer.size() - upgrader.m_send_offset;
        const char* send_ptr = upgrader.m_send_buffer.data() + upgrader.m_send_offset;
        upgrader.m_send_awaitable.emplace(session.m_socket.send(send_ptr, remaining));
        return upgrader.m_send_awaitable->await_suspend(handle);

    } else {
        upgrader.m_recv_awaitable.emplace(session.m_socket.readv(session.m_ring_buffer.getWriteIovecs()));
        return upgrader.m_recv_awaitable->await_suspend(handle);
    }
}

template<typename SocketType>
std::expected<bool, WsError> WsSessionUpgradeAwaitableImpl<SocketType>::await_resume() {
    auto& upgrader = *m_upgrader;
    auto& session = *upgrader.m_session;

    if (upgrader.m_state == WsSessionUpgraderImpl<SocketType>::State::Sending) {
        auto send_result = upgrader.m_send_awaitable->await_resume();
        upgrader.m_send_awaitable.reset();

        if (!send_result) {
            HTTP_LOG_ERROR("Failed to send upgrade request: {}", send_result.error().message());
            upgrader.m_state = WsSessionUpgraderImpl<SocketType>::State::Invalid;
            return std::unexpected(WsError(kWsConnectionClosed, "Failed to send upgrade request"));
        }

        size_t bytes_sent = send_result.value();
        upgrader.m_send_offset += bytes_sent;

        if (upgrader.m_send_offset < upgrader.m_send_buffer.size()) {
            return false;  // 继续发送
        }

        HTTP_LOG_INFO("Sent WebSocket upgrade request");
        upgrader.m_state = WsSessionUpgraderImpl<SocketType>::State::Receiving;
        return false;  // 继续接收响应

    } else if (upgrader.m_state == WsSessionUpgraderImpl<SocketType>::State::Receiving) {
        auto recv_result = upgrader.m_recv_awaitable->await_resume();
        upgrader.m_recv_awaitable.reset();

        if (!recv_result) {
            HTTP_LOG_ERROR("Failed to read upgrade response: {}", recv_result.error().message());
            upgrader.m_state = WsSessionUpgraderImpl<SocketType>::State::Invalid;
            return std::unexpected(WsError(kWsConnectionClosed, "Failed to read upgrade response"));
        }

        size_t bytes_read = recv_result.value();
        if (bytes_read == 0) {
            HTTP_LOG_ERROR("Connection closed while reading upgrade response");
            upgrader.m_state = WsSessionUpgraderImpl<SocketType>::State::Invalid;
            return std::unexpected(WsError(kWsConnectionClosed, "Connection closed"));
        }

        session.m_ring_buffer.produce(bytes_read);

        auto iovecs = session.m_ring_buffer.getReadIovecs();
        auto [error_code, consumed] = upgrader.m_upgrade_response.fromIOVec(iovecs);

        if (consumed > 0) {
            session.m_ring_buffer.consume(consumed);
        }

        if (error_code == kIncomplete || error_code == kHeaderInComplete) {
            return false;  // 需要更多数据
        }

        if (error_code != kNoError) {
            HTTP_LOG_ERROR("Failed to parse upgrade response: {}", static_cast<int>(error_code));
            upgrader.m_state = WsSessionUpgraderImpl<SocketType>::State::Invalid;
            return std::unexpected(WsError(kWsProtocolError, "Failed to parse upgrade response"));
        }

        // 验证升级响应
        if (upgrader.m_upgrade_response.header().code() != HttpStatusCode::SwitchingProtocol_101) {
            HTTP_LOG_ERROR("WebSocket upgrade failed. Status: {} {}",
                          static_cast<int>(upgrader.m_upgrade_response.header().code()),
                          httpStatusCodeToString(upgrader.m_upgrade_response.header().code()));
            upgrader.m_state = WsSessionUpgraderImpl<SocketType>::State::Invalid;
            return std::unexpected(WsError(kWsUpgradeFailed,
                "Upgrade failed with status " +
                std::to_string(static_cast<int>(upgrader.m_upgrade_response.header().code()))));
        }

        if (!upgrader.m_upgrade_response.header().headerPairs().hasKey("Sec-WebSocket-Accept")) {
            HTTP_LOG_ERROR("Missing Sec-WebSocket-Accept header in response");
            upgrader.m_state = WsSessionUpgraderImpl<SocketType>::State::Invalid;
            return std::unexpected(WsError(kWsUpgradeFailed, "Missing Sec-WebSocket-Accept header"));
        }

        std::string accept_key = upgrader.m_upgrade_response.header().headerPairs().getValue("Sec-WebSocket-Accept");
        std::string expected_accept = WsUpgrade::generateAcceptKey(upgrader.m_ws_key);

        if (accept_key != expected_accept) {
            HTTP_LOG_ERROR("Invalid Sec-WebSocket-Accept value");
            upgrader.m_state = WsSessionUpgraderImpl<SocketType>::State::Invalid;
            return std::unexpected(WsError(kWsUpgradeFailed, "Invalid Sec-WebSocket-Accept value"));
        }

        HTTP_LOG_INFO("WebSocket upgrade successful!");
        session.m_upgraded = true;
        upgrader.m_state = WsSessionUpgraderImpl<SocketType>::State::Invalid;

        return true;  // 升级完成

    } else {
        HTTP_LOG_ERROR("await_resume called in Invalid state");
        return std::unexpected(WsError(kWsProtocolError, "Invalid state"));
    }
}

// 类型别名 - WebSocket over TCP
using WsSessionUpgradeAwaitable = WsSessionUpgradeAwaitableImpl<TcpSocket>;
using WsSession = WsSessionImpl<TcpSocket>;

} // namespace galay::websocket

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/SslSocket.h"

namespace galay::websocket {

// 类型别名 - WebSocket over SSL (WSS)
using WssSessionUpgradeAwaitable = WsSessionUpgradeAwaitableImpl<galay::ssl::SslSocket>;
using WssSession = WsSessionImpl<galay::ssl::SslSocket>;

} // namespace galay::websocket
#endif

#endif // GALAY_WS_SESSION_H
