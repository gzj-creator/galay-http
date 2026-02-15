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
#include "galay-kernel/kernel/Awaitable.h"
#include "galay-kernel/kernel/IOHandlers.hpp"
#include <galay-utils/algorithm/Base64.hpp>
#include <string>
#include <optional>
#include <coroutine>

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/async/SslSocket.h"
#include "galay-http/kernel/SslRecvCompatAwaitable.h"
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
    WsSessionUpgraderImpl(WsSessionImpl<SocketType>* session)
        : m_session(session)
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
    WsSessionImpl<SocketType>* m_session;
};

/**
 * @brief WebSocket Session 升级等待体 - TcpSocket 版本（CustomAwaitable 链式 SEND+RECV）
 */
template<typename SocketType>
class WsSessionUpgradeAwaitableImpl<SocketType, false>
    : public galay::kernel::CustomAwaitable
    , public galay::kernel::TimeoutSupport<WsSessionUpgradeAwaitableImpl<SocketType, false>>
{
public:
    class UpgradeSendAwaitable : public galay::kernel::SendAwaitable
    {
    public:
        explicit UpgradeSendAwaitable(WsSessionUpgradeAwaitableImpl* owner)
            : galay::kernel::SendAwaitable(owner->m_upgrader->m_session->m_socket.controller(),
                                          nullptr,
                                          0)
            , m_owner(owner)
        {
        }

        void resetBuffer(const char* buffer, size_t length) {
            m_buffer = buffer;
            m_length = length;
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle) override {
            if (m_length == 0) {
                return true;
            }
            if (cqe == nullptr) {
                return false;
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

            m_buffer += sent;
            m_length -= sent;
            return m_length == 0;
        }
#else
        bool handleComplete(GHandle handle) override {
            while (m_length > 0) {
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

                m_buffer += sent;
                m_length -= sent;
            }
            return true;
        }
#endif

    private:
        WsSessionUpgradeAwaitableImpl* m_owner;
    };

    class UpgradeRecvAwaitable : public galay::kernel::RecvAwaitable
    {
    public:
        explicit UpgradeRecvAwaitable(WsSessionUpgradeAwaitableImpl* owner)
            : galay::kernel::RecvAwaitable(owner->m_upgrader->m_session->m_socket.controller(), nullptr, 0)
            , m_owner(owner)
        {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle) override {
            if (m_owner->parseUpgradeResponse()) {
                return true;
            }

            if (cqe == nullptr) {
                if (!prepareRecvWindow()) {
                    m_owner->setProtocolError("Upgrade response too large");
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
                m_owner->setProtocolError("Connection closed");
                return true;
            }

            m_owner->m_upgrader->m_session->m_ring_buffer.produce(bytes_read);

            if (m_owner->parseUpgradeResponse()) {
                return true;
            }

            if (!prepareRecvWindow()) {
                m_owner->setProtocolError("Upgrade response too large");
                return true;
            }
            return false;
        }
#else
        bool handleComplete(GHandle handle) override {
            while (true) {
                if (m_owner->parseUpgradeResponse()) {
                    return true;
                }

                if (!prepareRecvWindow()) {
                    m_owner->setProtocolError("Upgrade response too large");
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
                    m_owner->setProtocolError("Connection closed");
                    return true;
                }

                m_owner->m_upgrader->m_session->m_ring_buffer.produce(bytes_read);
            }
        }
#endif

    private:
        bool prepareRecvWindow() {
            auto write_iovecs = m_owner->m_upgrader->m_session->m_ring_buffer.getWriteIovecs();
            if (write_iovecs.empty()) {
                return false;
            }

            m_buffer = static_cast<char*>(write_iovecs[0].iov_base);
            m_length = write_iovecs[0].iov_len;
            return m_length > 0;
        }

        WsSessionUpgradeAwaitableImpl* m_owner;
    };

    WsSessionUpgradeAwaitableImpl(WsSessionUpgraderImpl<SocketType>* upgrader)
        : galay::kernel::CustomAwaitable(upgrader->m_session->m_socket.controller())
        , m_upgrader(upgrader)
        , m_send_awaitable(this)
        , m_recv_awaitable(this)
        , m_result(true)
    {
        if (m_upgrader->m_session->m_upgraded) {
            m_done = true;
            return;
        }

        initUpgradeRequest();
        m_send_awaitable.resetBuffer(m_send_buffer.data(), m_send_buffer.size());
        addTask(IOEventType::SEND, &m_send_awaitable);
        addTask(IOEventType::RECV, &m_recv_awaitable);
    }

    bool await_ready() const noexcept { return m_done; }
    using galay::kernel::CustomAwaitable::await_suspend;

    std::expected<bool, WsError> await_resume() {
        onCompleted();

        if (!m_result.has_value()) {
            auto& io_error = m_result.error();
            if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kTimeout)) {
                return std::unexpected(WsError(kWsConnectionError, "Upgrade timeout"));
            }
            if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
                return std::unexpected(WsError(kWsConnectionClosed, io_error.message()));
            }
            return std::unexpected(WsError(kWsConnectionError, io_error.message()));
        }

        if (m_error.has_value()) {
            return std::unexpected(std::move(*m_error));
        }

        return true;
    }

private:
    void initUpgradeRequest() {
        auto& session = *m_upgrader->m_session;
        m_ws_key = generateWebSocketKey();

        auto request = Http1_1RequestBuilder::get(session.m_url.path)
            .header("Host", session.m_url.host + ":" + std::to_string(session.m_url.port))
            .header("Upgrade", "websocket")
            .header("Connection", "Upgrade")
            .header("Sec-WebSocket-Key", m_ws_key)
            .header("Sec-WebSocket-Version", "13")
            .build();

        m_send_buffer = request.toString();
        HTTP_LOG_INFO("[ws] [upgrade] [send]");
    }

    bool parseUpgradeResponse() {
        auto& session = *m_upgrader->m_session;
        auto iovecs = session.m_ring_buffer.getReadIovecs();
        if (iovecs.empty()) {
            return false;
        }

        auto [error_code, consumed] = m_upgrade_response.fromIOVec(iovecs);
        if (consumed > 0) {
            session.m_ring_buffer.consume(consumed);
        }

        if (error_code == kIncomplete || error_code == kHeaderInComplete) {
            return false;
        }

        if (error_code != kNoError) {
            setProtocolError("Failed to parse upgrade response");
            return true;
        }

        if (!m_upgrade_response.isComplete()) {
            return false;
        }

        if (!validateUpgradeResponse()) {
            return true;
        }

        HTTP_LOG_INFO("[ws] [upgrade] [ok]");
        session.m_upgraded = true;
        m_done = true;
        return true;
    }

    bool validateUpgradeResponse() {
        if (m_upgrade_response.header().code() != HttpStatusCode::SwitchingProtocol_101) {
            return setUpgradeFailed("Upgrade failed with status " +
                                    std::to_string(static_cast<int>(m_upgrade_response.header().code())));
        }

        if (!m_upgrade_response.header().headerPairs().hasKey("Sec-WebSocket-Accept")) {
            return setUpgradeFailed("Missing Sec-WebSocket-Accept header");
        }

        std::string accept_key = m_upgrade_response.header().headerPairs().getValue("Sec-WebSocket-Accept");
        std::string expected_accept = WsUpgrade::generateAcceptKey(m_ws_key);
        if (accept_key != expected_accept) {
            return setUpgradeFailed("Invalid Sec-WebSocket-Accept value");
        }

        return true;
    }

    bool setUpgradeFailed(std::string message) {
        m_error = WsError(kWsUpgradeFailed, std::move(message));
        return false;
    }

    void setSendError(const galay::kernel::IOError& io_error) {
        m_error = WsError(kWsSendError, io_error.message());
    }

    void setRecvError(const galay::kernel::IOError& io_error) {
        if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
            m_error = WsError(kWsConnectionClosed, io_error.message());
            return;
        }
        m_error = WsError(kWsConnectionError, io_error.message());
    }

    void setProtocolError(std::string message) {
        m_error = WsError(kWsProtocolError, std::move(message));
    }

    WsSessionUpgraderImpl<SocketType>* m_upgrader;
    std::string m_ws_key;
    std::string m_send_buffer;
    HttpResponse m_upgrade_response;
    UpgradeSendAwaitable m_send_awaitable;
    UpgradeRecvAwaitable m_recv_awaitable;
    std::optional<WsError> m_error;
    bool m_done = false;

public:
    std::expected<bool, galay::kernel::IOError> m_result;
};

#ifdef GALAY_HTTP_SSL_ENABLED
/**
 * @brief WebSocket Session 升级等待体 - SslSocket 版本（CustomAwaitable 链式 SEND+RECV）
 */
template<typename SocketType>
class WsSessionUpgradeAwaitableImpl<SocketType, true>
    : public galay::kernel::CustomAwaitable
    , public galay::kernel::TimeoutSupport<WsSessionUpgradeAwaitableImpl<SocketType, true>>
{
public:
    using SendAwaitableType = decltype(std::declval<SocketType>().send(std::declval<const char*>(), std::declval<size_t>()));
    using RecvAwaitableType = galay::http::SslRecvCompatAwaitable;

    class UpgradeSendAwaitable : public SendAwaitableType
    {
    public:
        explicit UpgradeSendAwaitable(WsSessionUpgradeAwaitableImpl* owner)
            : SendAwaitableType(owner->m_upgrader->m_session->m_socket.send(owner->m_send_buffer.data(),
                                                                             owner->m_send_buffer.size()))
            , m_owner(owner)
        {
        }

        void resetBuffer(const char* buffer, size_t length) {
            this->m_plainBuffer = buffer;
            this->m_plainLength = length;
            this->m_plainOffset = 0;
            this->m_cipherLength = 0;
            this->m_sslResultSet = false;
            this->m_buffer = nullptr;
            this->m_length = 0;
            if (length == 0) {
                this->m_sslResult = size_t{0};
                this->m_sslResultSet = true;
                return;
            }
            this->fillNextSendChunk();
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override {
            if (m_owner->m_send_completed) {
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
                m_owner->setSslSendError(error);
                return true;
            }

            size_t sent = send_result.value();
            if (sent == 0) {
                return false;
            }

            m_owner->m_send_offset += sent;
            if (m_owner->m_send_offset >= m_owner->m_send_buffer.size()) {
                m_owner->m_send_completed = true;
                return true;
            }

            this->m_plainBuffer = m_owner->m_send_buffer.data() + m_owner->m_send_offset;
            this->m_plainLength = m_owner->m_send_buffer.size() - m_owner->m_send_offset;
            return false;
        }
#else
        bool handleComplete(GHandle handle) override {
            while (!m_owner->m_send_completed) {
                this->m_sslResultSet = false;
                const bool done = SendAwaitableType::handleComplete(handle);
                if (!done) {
                    return false;
                }

                auto send_result = std::move(this->m_sslResult);
                this->m_sslResultSet = false;
                if (!send_result) {
                    const auto& error = send_result.error();
                    m_owner->setSslSendError(error);
                    return true;
                }

                size_t sent = send_result.value();
                if (sent == 0) {
                    return false;
                }

                m_owner->m_send_offset += sent;
                if (m_owner->m_send_offset >= m_owner->m_send_buffer.size()) {
                    m_owner->m_send_completed = true;
                    return true;
                }

                this->m_plainBuffer = m_owner->m_send_buffer.data() + m_owner->m_send_offset;
                this->m_plainLength = m_owner->m_send_buffer.size() - m_owner->m_send_offset;
            }
            return true;
        }
#endif

    private:
        WsSessionUpgradeAwaitableImpl* m_owner;
    };

    class UpgradeRecvAwaitable : public RecvAwaitableType
    {
    public:
        explicit UpgradeRecvAwaitable(WsSessionUpgradeAwaitableImpl* owner)
            : RecvAwaitableType(owner->m_upgrader->m_session->m_socket.recv(owner->m_dummy_recv_buffer,
                                                                             sizeof(owner->m_dummy_recv_buffer)))
            , m_owner(owner)
        {
        }

#ifdef USE_IOURING
        bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override {
            if (m_owner->parseUpgradeResponse()) {
                return true;
            }

            if (!prepareRecvWindow()) {
                m_owner->setProtocolError("Upgrade response too large");
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
                m_owner->setSslRecvError(error);
                return true;
            }

            size_t bytes_read = recv_result.value().size();
            if (bytes_read == 0) {
                m_owner->setProtocolError("Connection closed");
                return true;
            }

            m_owner->m_upgrader->m_session->m_ring_buffer.produce(bytes_read);
            return m_owner->parseUpgradeResponse();
        }
#else
        bool handleComplete(GHandle handle) override {
            while (true) {
                if (m_owner->parseUpgradeResponse()) {
                    return true;
                }

                if (!prepareRecvWindow()) {
                    m_owner->setProtocolError("Upgrade response too large");
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
                    m_owner->setSslRecvError(error);
                    return true;
                }

                size_t bytes_read = recv_result.value().size();
                if (bytes_read == 0) {
                    m_owner->setProtocolError("Connection closed");
                    return true;
                }

                m_owner->m_upgrader->m_session->m_ring_buffer.produce(bytes_read);
            }
        }
#endif

    private:
        bool prepareRecvWindow() {
            auto write_iovecs = m_owner->m_upgrader->m_session->m_ring_buffer.getWriteIovecs();
            if (write_iovecs.empty()) {
                return false;
            }
            this->m_plainBuffer = static_cast<char*>(write_iovecs[0].iov_base);
            this->m_plainLength = write_iovecs[0].iov_len;
            return this->m_plainLength > 0;
        }

        WsSessionUpgradeAwaitableImpl* m_owner;
    };

    WsSessionUpgradeAwaitableImpl(WsSessionUpgraderImpl<SocketType>* upgrader)
        : galay::kernel::CustomAwaitable(upgrader->m_session->m_socket.controller())
        , m_upgrader(upgrader)
        , m_send_awaitable(this)
        , m_recv_awaitable(this)
        , m_result(true)
    {
        if (m_upgrader->m_session->m_upgraded) {
            m_done = true;
            return;
        }

        initUpgradeRequest();
        m_send_awaitable.resetBuffer(m_send_buffer.data(), m_send_buffer.size());
        addTask(IOEventType::SEND, &m_send_awaitable);
        addTask(IOEventType::RECV, &m_recv_awaitable);
    }

    bool await_ready() const noexcept { return m_done; }
    using galay::kernel::CustomAwaitable::await_suspend;

    std::expected<bool, WsError> await_resume() {
        if (m_done) {
            return true;
        }

        onCompleted();

        if (!m_result.has_value()) {
            auto& io_error = m_result.error();
            if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kTimeout)) {
                return std::unexpected(WsError(kWsConnectionError, "Upgrade timeout"));
            }
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
    void initUpgradeRequest() {
        auto& session = *m_upgrader->m_session;
        m_ws_key = generateWebSocketKey();

        auto request = Http1_1RequestBuilder::get(session.m_url.path)
            .header("Host", session.m_url.host + ":" + std::to_string(session.m_url.port))
            .header("Upgrade", "websocket")
            .header("Connection", "Upgrade")
            .header("Sec-WebSocket-Key", m_ws_key)
            .header("Sec-WebSocket-Version", "13")
            .build();

        m_send_buffer = request.toString();
        HTTP_LOG_INFO("[ws] [upgrade] [send]");
    }

    bool parseUpgradeResponse() {
        auto& session = *m_upgrader->m_session;
        auto iovecs = session.m_ring_buffer.getReadIovecs();
        if (iovecs.empty()) {
            return false;
        }

        auto [error_code, consumed] = m_upgrade_response.fromIOVec(iovecs);
        if (consumed > 0) {
            session.m_ring_buffer.consume(consumed);
        }

        if (error_code == kIncomplete || error_code == kHeaderInComplete) {
            return false;
        }

        if (error_code != kNoError) {
            setProtocolError("Failed to parse upgrade response");
            return true;
        }

        if (!m_upgrade_response.isComplete()) {
            return false;
        }

        if (!validateUpgradeResponse()) {
            return true;
        }

        session.m_upgraded = true;
        HTTP_LOG_INFO("[ws] [upgrade] [ok]");
        m_done = true;
        return true;
    }

    bool validateUpgradeResponse() {
        if (m_upgrade_response.header().code() != HttpStatusCode::SwitchingProtocol_101) {
            return setUpgradeFailed("Upgrade failed with status " +
                                    std::to_string(static_cast<int>(m_upgrade_response.header().code())));
        }

        if (!m_upgrade_response.header().headerPairs().hasKey("Sec-WebSocket-Accept")) {
            return setUpgradeFailed("Missing Sec-WebSocket-Accept header");
        }

        std::string accept_key = m_upgrade_response.header().headerPairs().getValue("Sec-WebSocket-Accept");
        std::string expected_accept = WsUpgrade::generateAcceptKey(m_ws_key);
        if (accept_key != expected_accept) {
            return setUpgradeFailed("Invalid Sec-WebSocket-Accept value");
        }

        return true;
    }

    bool setUpgradeFailed(std::string message) {
        m_ws_error = WsError(kWsUpgradeFailed, std::move(message));
        return false;
    }

    void setSslSendError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            m_ws_error = WsError(kWsConnectionClosed, "Connection closed by peer");
            return;
        }
        m_ws_error = WsError(kWsSendError, error.message());
    }

    void setSslRecvError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            m_ws_error = WsError(kWsConnectionClosed, "Connection closed by peer");
            return;
        }
        m_ws_error = WsError(kWsConnectionError, error.message());
    }

    void setProtocolError(std::string message) {
        m_ws_error = WsError(kWsProtocolError, std::move(message));
    }

    WsSessionUpgraderImpl<SocketType>* m_upgrader;
    std::string m_ws_key;
    std::string m_send_buffer;
    HttpResponse m_upgrade_response;
    size_t m_send_offset = 0;
    bool m_send_completed = false;
    char m_dummy_recv_buffer[1]{0};
    UpgradeSendAwaitable m_send_awaitable;
    UpgradeRecvAwaitable m_recv_awaitable;
    std::optional<WsError> m_ws_error;
    bool m_done = false;

public:
    std::expected<bool, galay::kernel::IOError> m_result;
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

// 类型别名 - WebSocket over TCP
using WsSessionUpgradeAwaitable = WsSessionUpgradeAwaitableImpl<TcpSocket>;
using WsSession = WsSessionImpl<TcpSocket>;

} // namespace galay::websocket

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/async/SslSocket.h"

namespace galay::websocket {

// 类型别名 - WebSocket over SSL (WSS)
using WssSessionUpgradeAwaitable = WsSessionUpgradeAwaitableImpl<galay::ssl::SslSocket>;
using WssSession = WsSessionImpl<galay::ssl::SslSocket>;

} // namespace galay::websocket
#endif

#endif // GALAY_WS_SESSION_H
