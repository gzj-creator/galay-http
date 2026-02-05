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

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/SslSocket.h"
#endif

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

template<typename SocketType, bool IsSsl = is_ssl_socket_v<SocketType>>
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

    friend class WsSessionUpgradeAwaitableImpl<SocketType, false>;
#ifdef GALAY_HTTP_SSL_ENABLED
    friend class WsSessionUpgradeAwaitableImpl<SocketType, true>;
#endif
    friend class WsSessionUpgraderImpl<SocketType>;

private:
    enum class State {
        Invalid,
        Sending,
        Receiving
    };

    WsSessionImpl<SocketType>* m_session;
    State m_state;

    std::optional<SendAwaitableType> m_send_awaitable;

    std::string m_send_buffer;
    size_t m_send_offset;
    std::string m_ws_key;
    HttpResponse m_upgrade_response;
};

/**
 * @brief WebSocket Session 升级等待体 - TcpSocket 版本（使用 readv）
 */
template<typename SocketType>
class WsSessionUpgradeAwaitableImpl<SocketType, false>
{
public:
    using ReadvAwaitableType = decltype(std::declval<SocketType>().readv(std::declval<std::vector<iovec>>()));

    WsSessionUpgradeAwaitableImpl(WsSessionUpgraderImpl<SocketType>* upgrader)
        : m_upgrader(upgrader)
    {
    }

    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> handle);
    std::expected<bool, WsError> await_resume();

private:
    WsSessionUpgraderImpl<SocketType>* m_upgrader;
    std::optional<ReadvAwaitableType> m_recv_awaitable;
};

#ifdef GALAY_HTTP_SSL_ENABLED
/**
 * @brief WebSocket Session 升级等待体 - SslSocket 版本（使用 recv）
 */
template<typename SocketType>
class WsSessionUpgradeAwaitableImpl<SocketType, true>
{
public:
    using RecvAwaitableType = decltype(std::declval<SocketType>().recv(std::declval<char*>(), std::declval<size_t>()));

    WsSessionUpgradeAwaitableImpl(WsSessionUpgraderImpl<SocketType>* upgrader)
        : m_upgrader(upgrader)
    {
    }

    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> handle);
    std::expected<bool, WsError> await_resume();

private:
    WsSessionUpgraderImpl<SocketType>* m_upgrader;
    std::optional<RecvAwaitableType> m_recv_awaitable;
};
#endif

/**
 * @brief WebSocket会话模板类
 * @details 持有 socket、ring_buffer、reader 和 writer，负责WebSocket升级和通信
 */
template<typename SocketType>
class WsSessionImpl
{
public:
    using SendAwaitableType = decltype(std::declval<SocketType>().send(std::declval<const char*>(), std::declval<size_t>()));

    WsSessionImpl(SocketType& socket,
                  const WsUrl& url,
                  const WsWriterSetting& writer_setting,
                  size_t ring_buffer_size = 8192,
                  const WsReaderSetting& reader_setting = WsReaderSetting())
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

    friend class WsSessionUpgradeAwaitableImpl<SocketType, false>;
#ifdef GALAY_HTTP_SSL_ENABLED
    friend class WsSessionUpgradeAwaitableImpl<SocketType, true>;
#endif
    friend class WsSessionUpgraderImpl<SocketType>;

private:
    SocketType& m_socket;
    const WsUrl& m_url;
    RingBuffer m_ring_buffer;
    WsReaderImpl<SocketType> m_reader;
    WsWriterImpl<SocketType> m_writer;
    bool m_upgraded;
};

// WsSessionUpgradeAwaitableImpl<SocketType, false> 的实现 - TcpSocket 版本
template<typename SocketType>
bool WsSessionUpgradeAwaitableImpl<SocketType, false>::await_suspend(std::coroutine_handle<> handle) {
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

        HTTP_LOG_INFO("[ws] [upgrade] [send]");

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
        m_recv_awaitable.emplace(session.m_socket.readv(session.m_ring_buffer.getWriteIovecs()));
        return m_recv_awaitable->await_suspend(handle);
    }
}

template<typename SocketType>
std::expected<bool, WsError> WsSessionUpgradeAwaitableImpl<SocketType, false>::await_resume() {
    auto& upgrader = *m_upgrader;
    auto& session = *upgrader.m_session;

    if (upgrader.m_state == WsSessionUpgraderImpl<SocketType>::State::Sending) {
        auto send_result = upgrader.m_send_awaitable->await_resume();
        upgrader.m_send_awaitable.reset();

        if (!send_result) {
            HTTP_LOG_ERROR("[ws] [upgrade] [send-fail] [{}]", send_result.error().message());
            upgrader.m_state = WsSessionUpgraderImpl<SocketType>::State::Invalid;
            return std::unexpected(WsError(kWsConnectionClosed, "Failed to send upgrade request"));
        }

        size_t bytes_sent = send_result.value();
        upgrader.m_send_offset += bytes_sent;

        if (upgrader.m_send_offset < upgrader.m_send_buffer.size()) {
            return false;  // 继续发送
        }

        HTTP_LOG_INFO("[ws] [upgrade] [sent]");
        upgrader.m_state = WsSessionUpgraderImpl<SocketType>::State::Receiving;
        return false;  // 继续接收响应

    } else if (upgrader.m_state == WsSessionUpgraderImpl<SocketType>::State::Receiving) {
        auto recv_result = m_recv_awaitable->await_resume();
        m_recv_awaitable.reset();

        if (!recv_result) {
            HTTP_LOG_ERROR("[ws] [upgrade] [recv-fail] [{}]", recv_result.error().message());
            upgrader.m_state = WsSessionUpgraderImpl<SocketType>::State::Invalid;
            return std::unexpected(WsError(kWsConnectionClosed, "Failed to read upgrade response"));
        }

        size_t bytes_read = recv_result.value();
        if (bytes_read == 0) {
            HTTP_LOG_ERROR("[ws] [upgrade] [conn-closed]");
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
            HTTP_LOG_ERROR("[ws] [upgrade] [parse-fail] [code={}]",
                          static_cast<int>(error_code));
            upgrader.m_state = WsSessionUpgraderImpl<SocketType>::State::Invalid;
            return std::unexpected(WsError(kWsProtocolError, "Failed to parse upgrade response"));
        }

        // 验证升级响应
        if (upgrader.m_upgrade_response.header().code() != HttpStatusCode::SwitchingProtocol_101) {
            HTTP_LOG_ERROR("[ws] [upgrade] [fail] [{}] [{}]",
                          static_cast<int>(upgrader.m_upgrade_response.header().code()),
                          httpStatusCodeToString(upgrader.m_upgrade_response.header().code()));
            upgrader.m_state = WsSessionUpgraderImpl<SocketType>::State::Invalid;
            return std::unexpected(WsError(kWsUpgradeFailed,
                "Upgrade failed with status " +
                std::to_string(static_cast<int>(upgrader.m_upgrade_response.header().code()))));
        }

        if (!upgrader.m_upgrade_response.header().headerPairs().hasKey("Sec-WebSocket-Accept")) {
            HTTP_LOG_ERROR("[ws] [upgrade] [accept-missing]");
            upgrader.m_state = WsSessionUpgraderImpl<SocketType>::State::Invalid;
            return std::unexpected(WsError(kWsUpgradeFailed, "Missing Sec-WebSocket-Accept header"));
        }

        std::string accept_key = upgrader.m_upgrade_response.header().headerPairs().getValue("Sec-WebSocket-Accept");
        std::string expected_accept = WsUpgrade::generateAcceptKey(upgrader.m_ws_key);

        if (accept_key != expected_accept) {
            HTTP_LOG_ERROR("[ws] [upgrade] [accept-invalid]");
            upgrader.m_state = WsSessionUpgraderImpl<SocketType>::State::Invalid;
            return std::unexpected(WsError(kWsUpgradeFailed, "Invalid Sec-WebSocket-Accept value"));
        }

        HTTP_LOG_INFO("[ws] [upgrade] [ok]");
        session.m_upgraded = true;
        upgrader.m_state = WsSessionUpgraderImpl<SocketType>::State::Invalid;

        return true;  // 升级完成

    } else {
        HTTP_LOG_ERROR("[state] [invalid] [await-resume]");
        return std::unexpected(WsError(kWsProtocolError, "Invalid state"));
    }
}

#ifdef GALAY_HTTP_SSL_ENABLED
// WsSessionUpgradeAwaitableImpl<SocketType, true> 的实现 - SslSocket 版本
template<typename SocketType>
bool WsSessionUpgradeAwaitableImpl<SocketType, true>::await_suspend(std::coroutine_handle<> handle) {
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

        HTTP_LOG_INFO("[ws] [upgrade] [send]");

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
        if (!m_recv_awaitable) {
            auto write_iovecs = session.m_ring_buffer.getWriteIovecs();
            if (!write_iovecs.empty()) {
                m_recv_awaitable.emplace(session.m_socket.recv(
                    static_cast<char*>(write_iovecs[0].iov_base),
                    write_iovecs[0].iov_len));
            }
        }
        return m_recv_awaitable->await_suspend(handle);
    }
}

template<typename SocketType>
std::expected<bool, WsError> WsSessionUpgradeAwaitableImpl<SocketType, true>::await_resume() {
    auto& upgrader = *m_upgrader;
    auto& session = *upgrader.m_session;

    if (upgrader.m_state == WsSessionUpgraderImpl<SocketType>::State::Sending) {
        auto send_result = upgrader.m_send_awaitable->await_resume();
        upgrader.m_send_awaitable.reset();

        if (!send_result) {
            HTTP_LOG_ERROR("[ws] [upgrade] [send-fail] [{}]", send_result.error().message());
            upgrader.m_state = WsSessionUpgraderImpl<SocketType>::State::Invalid;
            return std::unexpected(WsError(kWsConnectionClosed, "Failed to send upgrade request"));
        }

        size_t bytes_sent = send_result.value();
        upgrader.m_send_offset += bytes_sent;

        if (upgrader.m_send_offset < upgrader.m_send_buffer.size()) {
            return false;  // 继续发送
        }

        HTTP_LOG_INFO("[ws] [upgrade] [sent]");
        upgrader.m_state = WsSessionUpgraderImpl<SocketType>::State::Receiving;
        return false;  // 继续接收响应

    } else if (upgrader.m_state == WsSessionUpgraderImpl<SocketType>::State::Receiving) {
        auto recv_result = m_recv_awaitable->await_resume();
        m_recv_awaitable.reset();

        if (!recv_result) {
            auto& error = recv_result.error();
            // SSL_ERROR_WANT_READ (2) 或 SSL_ERROR_WANT_WRITE (3) 表示需要重试
            if (error.sslError() == SSL_ERROR_WANT_READ || error.sslError() == SSL_ERROR_WANT_WRITE) {
                HTTP_LOG_DEBUG("[ssl] [retry]");
                return false;  // 返回 false 表示需要继续读取
            }
            HTTP_LOG_ERROR("[ws] [upgrade] [recv-fail] [{}]", recv_result.error().message());
            upgrader.m_state = WsSessionUpgraderImpl<SocketType>::State::Invalid;
            return std::unexpected(WsError(kWsConnectionClosed, "Failed to read upgrade response"));
        }

        size_t bytes_read = recv_result.value().size();
        if (bytes_read == 0) {
            HTTP_LOG_ERROR("[ws] [upgrade] [conn-closed]");
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
            HTTP_LOG_ERROR("[ws] [upgrade] [parse-fail] [code={}]",
                          static_cast<int>(error_code));
            upgrader.m_state = WsSessionUpgraderImpl<SocketType>::State::Invalid;
            return std::unexpected(WsError(kWsProtocolError, "Failed to parse upgrade response"));
        }

        // 验证升级响应
        if (upgrader.m_upgrade_response.header().code() != HttpStatusCode::SwitchingProtocol_101) {
            HTTP_LOG_ERROR("[ws] [upgrade] [fail] [{}] [{}]",
                          static_cast<int>(upgrader.m_upgrade_response.header().code()),
                          httpStatusCodeToString(upgrader.m_upgrade_response.header().code()));
            upgrader.m_state = WsSessionUpgraderImpl<SocketType>::State::Invalid;
            return std::unexpected(WsError(kWsUpgradeFailed,
                "Upgrade failed with status " +
                std::to_string(static_cast<int>(upgrader.m_upgrade_response.header().code()))));
        }

        if (!upgrader.m_upgrade_response.header().headerPairs().hasKey("Sec-WebSocket-Accept")) {
            HTTP_LOG_ERROR("[ws] [upgrade] [accept-missing]");
            upgrader.m_state = WsSessionUpgraderImpl<SocketType>::State::Invalid;
            return std::unexpected(WsError(kWsUpgradeFailed, "Missing Sec-WebSocket-Accept header"));
        }

        std::string accept_key = upgrader.m_upgrade_response.header().headerPairs().getValue("Sec-WebSocket-Accept");
        std::string expected_accept = WsUpgrade::generateAcceptKey(upgrader.m_ws_key);

        if (accept_key != expected_accept) {
            HTTP_LOG_ERROR("[ws] [upgrade] [accept-invalid]");
            upgrader.m_state = WsSessionUpgraderImpl<SocketType>::State::Invalid;
            return std::unexpected(WsError(kWsUpgradeFailed, "Invalid Sec-WebSocket-Accept value"));
        }

        HTTP_LOG_INFO("[ws] [upgrade] [ok]");
        session.m_upgraded = true;
        upgrader.m_state = WsSessionUpgraderImpl<SocketType>::State::Invalid;

        return true;  // 升级完成

    } else {
        HTTP_LOG_ERROR("[state] [invalid] [await-resume]");
        return std::unexpected(WsError(kWsProtocolError, "Invalid state"));
    }
}
#endif

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
