#ifndef GALAY_WS_CLIENT_H
#define GALAY_WS_CLIENT_H

#include "WsSession.h"
#include "WsConn.h"
#include "WsUpgrade.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/kernel/http/HttpLog.h"
#include "galay-http/utils/Http1_1RequestBuilder.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/common/Buffer.h"
#include <galay-utils/algorithm/Base64.hpp>
#include "galay-http/protoc/http/HttpHeader.h"
#include <string>
#include <optional>
#include <expected>
#include <memory>
#include <regex>
#include <random>
#include <type_traits>

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/async/SslSocket.h"
#endif

namespace galay::websocket
{

using namespace galay::async;
using namespace galay::kernel;
using namespace galay::http;

/**
 * @brief WebSocket URL 解析结果
 */
struct WsUrl {
    std::string scheme;
    std::string host;
    int port;
    std::string path;
    bool is_secure;

    static std::optional<WsUrl> parse(const std::string& url) {
        std::regex url_regex(R"(^(ws|wss)://([^:/]+)(?::(\d+))?(/.*)?$)", std::regex::icase);
        std::smatch matches;

        if (!std::regex_match(url, matches, url_regex)) {
            HTTP_LOG_ERROR("[url] [invalid] [{}]", url);
            return std::nullopt;
        }

        WsUrl result;
        result.scheme = matches[1].str();
        result.host = matches[2].str();
        result.is_secure = (result.scheme == "wss" || result.scheme == "WSS");

        if (matches[3].matched) {
            try {
                result.port = std::stoi(matches[3].str());
            } catch (...) {
                HTTP_LOG_ERROR("[url] [port-invalid] [{}]", url);
                return std::nullopt;
            }
        } else {
            result.port = result.is_secure ? 443 : 80;
        }

        if (matches[4].matched) {
            result.path = matches[4].str();
        } else {
            result.path = "/";
        }

        return result;
    }
};

// 辅助函数
inline std::string generateWebSocketKey() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);

    unsigned char random_bytes[16];
    for (int i = 0; i < 16; i++) {
        random_bytes[i] = static_cast<unsigned char>(dis(gen));
    }

    return galay::utils::Base64Util::Base64Encode(random_bytes, 16);
}

/**
 * @brief WebSocket 客户端配置
 */
template<typename SocketType>
class WsClientImpl;

template<typename SocketType>
class WsUpgraderImpl;

struct WsClientConfig
{
    HeaderPair::NormalizeMode header_mode = HeaderPair::NormalizeMode::Canonical;
};

class WsClientBuilder {
public:
    WsClientBuilder& headerMode(HeaderPair::NormalizeMode v) { m_config.header_mode = v; return *this; }
    WsClientImpl<TcpSocket> build() const;
    WsClientConfig buildConfig() const                       { return m_config; }
private:
    WsClientConfig m_config;
};

/**
 * @brief WebSocket 升级等待体
 *
 * 这个类实现 awaitable 接口，使用 WsUpgraderImpl 中维护的状态
 */
template<typename SocketType>
class WsUpgradeAwaitableImpl
{
public:
    using SendAwaitableType = decltype(std::declval<SocketType>().send(std::declval<const char*>(), std::declval<size_t>()));
    using ReadvAwaitableType = decltype(std::declval<SocketType>().readv(std::declval<std::vector<iovec>>()));

    WsUpgradeAwaitableImpl(WsUpgraderImpl<SocketType>* upgrader)
        : m_upgrader(upgrader)
    {
    }

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle);
    std::expected<bool, WsError> await_resume();

private:
    WsUpgraderImpl<SocketType>* m_upgrader;
};

/**
 * @brief WebSocket 客户端升级器模板类
 *
 * 这个类管理 WebSocket 升级过程的状态。
 * 通过 operator() 返回 awaitable 对象进行升级操作。
 *
 * 使用示例：
 * @code
 * auto upgrader = client.upgrade();
 * auto result = co_await upgrader();
 * if (!result) {
 *     // 处理错误
 * } else if (result.value()) {
 *     // 升级完成
 * }
 * @endcode
 */
template<typename SocketType>
class WsUpgraderImpl
{
public:
    WsUpgraderImpl(SocketType* socket,
                   RingBuffer* ring_buffer,
                   const WsUrl& url,
                   const WsReaderSetting& reader_setting,
                   const WsWriterSetting& writer_setting,
                   std::unique_ptr<WsConnImpl<SocketType>>* ws_conn_ptr)
        : m_socket(socket)
        , m_ring_buffer(ring_buffer)
        , m_url(url)
        , m_reader_setting(reader_setting)
        , m_writer_setting(writer_setting)
        , m_ws_conn_ptr(ws_conn_ptr)
    {
    }

    /**
     * @brief 返回升级等待体
     * @return 可以 co_await 的等待体对象
     */
    WsUpgradeAwaitableImpl<SocketType> operator()() {
        return WsUpgradeAwaitableImpl<SocketType>(this);
    }

    friend class WsUpgradeAwaitableImpl<SocketType>;

private:
    // 引用 WsClient 的资源（不拥有所有权）
    SocketType* m_socket;
    RingBuffer* m_ring_buffer;
    const WsUrl& m_url;
    const WsReaderSetting& m_reader_setting;
    const WsWriterSetting& m_writer_setting;
    std::unique_ptr<WsConnImpl<SocketType>>* m_ws_conn_ptr;
};

// WsUpgradeAwaitableImpl<TcpSocket> 特化：使用 CustomAwaitable 链式 SEND+RECV
template<>
class WsUpgradeAwaitableImpl<TcpSocket>
    : public galay::kernel::CustomAwaitable
    , public galay::kernel::TimeoutSupport<WsUpgradeAwaitableImpl<TcpSocket>>
{
public:
    class UpgradeSendAwaitable : public galay::kernel::SendAwaitable
    {
    public:
        explicit UpgradeSendAwaitable(WsUpgradeAwaitableImpl* owner)
            : galay::kernel::SendAwaitable(owner->m_upgrader->m_socket->controller(), nullptr, 0)
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
        WsUpgradeAwaitableImpl* m_owner;
    };

    class UpgradeRecvAwaitable : public galay::kernel::RecvAwaitable
    {
    public:
        explicit UpgradeRecvAwaitable(WsUpgradeAwaitableImpl* owner)
            : galay::kernel::RecvAwaitable(owner->m_upgrader->m_socket->controller(), nullptr, 0)
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

            m_owner->m_upgrader->m_ring_buffer->produce(bytes_read);

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

                m_owner->m_upgrader->m_ring_buffer->produce(bytes_read);
            }
        }
#endif

    private:
        bool prepareRecvWindow() {
            auto write_iovecs = m_owner->m_upgrader->m_ring_buffer->getWriteIovecs();
            if (write_iovecs.empty()) {
                return false;
            }

            m_buffer = static_cast<char*>(write_iovecs[0].iov_base);
            m_length = write_iovecs[0].iov_len;
            return m_length > 0;
        }

        WsUpgradeAwaitableImpl* m_owner;
    };

    explicit WsUpgradeAwaitableImpl(WsUpgraderImpl<TcpSocket>* upgrader)
        : galay::kernel::CustomAwaitable(upgrader->m_socket->controller())
        , m_upgrader(upgrader)
        , m_send_awaitable(this)
        , m_recv_awaitable(this)
        , m_result(true)
    {
        if (m_upgrader->m_ws_conn_ptr && *m_upgrader->m_ws_conn_ptr) {
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
        m_ws_key = generateWebSocketKey();

        auto request = Http1_1RequestBuilder::get(m_upgrader->m_url.path)
            .host(m_upgrader->m_url.host + ":" + std::to_string(m_upgrader->m_url.port))
            .header("Connection", "Upgrade")
            .header("Upgrade", "websocket")
            .header("Sec-WebSocket-Version", "13")
            .header("Sec-WebSocket-Key", m_ws_key)
            .build();

        m_send_buffer = request.toString();
        HTTP_LOG_INFO("[ws] [upgrade] [send]");
    }

    bool parseUpgradeResponse() {
        auto read_iovecs = m_upgrader->m_ring_buffer->getReadIovecs();
        if (read_iovecs.empty()) {
            return false;
        }

        auto [error_code, consumed] = m_upgrade_response.fromIOVec(read_iovecs);
        if (consumed > 0) {
            m_upgrader->m_ring_buffer->consume(consumed);
        }

        if (error_code == HttpErrorCode::kIncomplete || error_code == HttpErrorCode::kHeaderInComplete) {
            return false;
        }

        if (error_code != HttpErrorCode::kNoError) {
            setProtocolError("Failed to parse upgrade response");
            return true;
        }

        if (!m_upgrade_response.isComplete()) {
            return false;
        }

        if (!validateUpgradeResponse()) {
            return true;
        }

        *m_upgrader->m_ws_conn_ptr = std::make_unique<WsConnImpl<TcpSocket>>(
            std::move(*m_upgrader->m_socket),
            std::move(*m_upgrader->m_ring_buffer),
            false
        );

        HTTP_LOG_INFO("[ws] [conn] [ready]");
        m_done = true;
        return true;
    }

    bool validateUpgradeResponse() {
        if (m_upgrade_response.header().code() != HttpStatusCode::SwitchingProtocol_101) {
            m_error = WsError(kWsUpgradeFailed,
                "Upgrade failed with status " +
                std::to_string(static_cast<int>(m_upgrade_response.header().code())));
            return false;
        }

        if (!m_upgrade_response.header().headerPairs().hasKey("Sec-WebSocket-Accept")) {
            m_error = WsError(kWsUpgradeFailed, "Missing Sec-WebSocket-Accept header");
            return false;
        }

        std::string accept_key = m_upgrade_response.header().headerPairs().getValue("Sec-WebSocket-Accept");
        std::string expected_accept = WsUpgrade::generateAcceptKey(m_ws_key);
        if (accept_key != expected_accept) {
            m_error = WsError(kWsUpgradeFailed, "Invalid Sec-WebSocket-Accept value");
            return false;
        }

        HTTP_LOG_INFO("[ws] [upgrade] [ok]");
        return true;
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

    WsUpgraderImpl<TcpSocket>* m_upgrader;
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

/**
 * @brief WebSocket 客户端模板类
 * @details 只负责连接，通过getSession()获取Session进行通信
 */
template<typename SocketType>
class WsClientImpl
{
public:
    explicit WsClientImpl(const WsClientConfig& config = WsClientConfig())
        : m_socket(nullptr)
        , m_config(config)
    {
    }

    ~WsClientImpl() = default;

    WsClientImpl(const WsClientImpl&) = delete;
    WsClientImpl& operator=(const WsClientImpl&) = delete;
    WsClientImpl(WsClientImpl&&) = delete;
    WsClientImpl& operator=(WsClientImpl&&) = delete;

    auto connect(const std::string& url) {
        auto parsed_url = WsUrl::parse(url);
        if (!parsed_url) {
            throw std::runtime_error("Invalid WebSocket URL: " + url);
        }

        m_url = parsed_url.value();

        if constexpr (std::is_same_v<SocketType, TcpSocket>) {
            if (m_url.is_secure) {
                throw std::runtime_error("WSS requires WssClient");
            }
        }

        HTTP_LOG_INFO("[connect] [ws] [{}:{}{}]",
                     m_url.host, m_url.port, m_url.path);

        m_socket = std::make_unique<SocketType>(IPType::IPV4);

        auto nonblock_result = m_socket->option().handleNonBlock();
        if (!nonblock_result) {
            throw std::runtime_error("Failed to set non-blocking: " + nonblock_result.error().message());
        }

        Host server_host(IPType::IPV4, m_url.host, m_url.port);
        return m_socket->connect(server_host);
    }

    /**
     * @brief 获取 WebSocket Session 用于升级和通信
     * @return WsSessionImpl 对象
     * @note 必须在 connect() 成功后调用
     */
    WsSessionImpl<SocketType> getSession( const WsWriterSetting& writer_setting,
                                          size_t ring_buffer_size = 8192,
                                          const WsReaderSetting& reader_setting = WsReaderSetting()) {
        if (!m_socket) {
            throw std::runtime_error("WsClient not connected. Call connect() first.");
        }
        return WsSessionImpl<SocketType>(*m_socket, m_url, writer_setting, ring_buffer_size, reader_setting);
    }

    auto close() {
        if (!m_socket) {
            throw std::runtime_error("WsClient not connected");
        }
        return m_socket->close();
    }

    /**
     * @brief 获取底层 Socket（用于 SSL 握手等操作）
     * @return SocketType 指针，如果未连接则返回 nullptr
     */
    SocketType* getSocket() {
        return m_socket.get();
    }

    /**
     * @brief SSL 握手（仅对 SslSocket 有效）
     * @return 握手等待体
     * @note 必须在 connect() 成功后调用
     */
    auto handshake() {
        if (!m_socket) {
            throw std::runtime_error("WsClient not connected. Call connect() first.");
        }
#ifdef GALAY_HTTP_SSL_ENABLED
        if constexpr (std::is_same_v<SocketType, galay::ssl::SslSocket>) {
            return m_socket->handshake();
        }
#endif
        return m_socket->handshake();
    }

    /**
     * @brief 检查 SSL 握手是否完成（仅对 SslSocket 有效）
     */
    bool isHandshakeCompleted() const {
        if (!m_socket) return false;
        if constexpr (requires { m_socket->isHandshakeCompleted(); }) {
            return m_socket->isHandshakeCompleted();
        }
        return true;  // 非 SSL socket 总是返回 true
    }

    const WsUrl& url() const { return m_url; }

protected:
    std::unique_ptr<SocketType> m_socket;
    WsClientConfig m_config;
    WsUrl m_url;
};

// 类型别名 - WebSocket over TCP
using WsUpgrader = WsUpgraderImpl<TcpSocket>;
using WsClient = WsClientImpl<TcpSocket>;
inline WsClient WsClientBuilder::build() const { return WsClient(m_config); }

} // namespace galay::websocket

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/async/SslSocket.h"
#include "galay-ssl/ssl/SslContext.h"
#include "galay-http/kernel/SslRecvCompatAwaitable.h"

namespace galay::websocket {

template<>
class WsUpgradeAwaitableImpl<galay::ssl::SslSocket>
    : public galay::kernel::CustomAwaitable
    , public galay::kernel::TimeoutSupport<WsUpgradeAwaitableImpl<galay::ssl::SslSocket>>
{
public:
    using SocketType = galay::ssl::SslSocket;
    using SendAwaitableType = decltype(std::declval<SocketType>().send(std::declval<const char*>(), std::declval<size_t>()));
    using RecvAwaitableType = galay::http::SslRecvCompatAwaitable;

    class UpgradeSendAwaitable : public SendAwaitableType
    {
    public:
        explicit UpgradeSendAwaitable(WsUpgradeAwaitableImpl* owner)
            : SendAwaitableType(owner->m_upgrader->m_socket->send(owner->m_send_buffer.data(),
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

            const size_t sent = send_result.value();
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

                const size_t sent = send_result.value();
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
        WsUpgradeAwaitableImpl* m_owner;
    };

    class UpgradeRecvAwaitable : public RecvAwaitableType
    {
    public:
        explicit UpgradeRecvAwaitable(WsUpgradeAwaitableImpl* owner)
            : RecvAwaitableType(owner->m_upgrader->m_socket->recv(owner->m_dummy_recv_buffer,
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

            const size_t bytes_read = recv_result.value().size();
            if (bytes_read == 0) {
                m_owner->setProtocolError("Connection closed");
                return true;
            }

            m_owner->m_upgrader->m_ring_buffer->produce(bytes_read);
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

                const size_t bytes_read = recv_result.value().size();
                if (bytes_read == 0) {
                    m_owner->setProtocolError("Connection closed");
                    return true;
                }

                m_owner->m_upgrader->m_ring_buffer->produce(bytes_read);
            }
        }
#endif

    private:
        bool prepareRecvWindow() {
            auto write_iovecs = m_owner->m_upgrader->m_ring_buffer->getWriteIovecs();
            if (write_iovecs.empty()) {
                return false;
            }
            this->m_plainBuffer = static_cast<char*>(write_iovecs[0].iov_base);
            this->m_plainLength = write_iovecs[0].iov_len;
            return this->m_plainLength > 0;
        }

        WsUpgradeAwaitableImpl* m_owner;
    };

    explicit WsUpgradeAwaitableImpl(WsUpgraderImpl<SocketType>* upgrader)
        : galay::kernel::CustomAwaitable(upgrader->m_socket->controller())
        , m_upgrader(upgrader)
        , m_send_awaitable(this)
        , m_recv_awaitable(this)
        , m_result(true)
    {
        if (m_upgrader->m_ws_conn_ptr && *m_upgrader->m_ws_conn_ptr) {
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

        if (m_error.has_value()) {
            return std::unexpected(std::move(*m_error));
        }

        return true;
    }

private:
    void initUpgradeRequest() {
        m_ws_key = generateWebSocketKey();

        auto request = Http1_1RequestBuilder::get(m_upgrader->m_url.path)
            .host(m_upgrader->m_url.host + ":" + std::to_string(m_upgrader->m_url.port))
            .header("Connection", "Upgrade")
            .header("Upgrade", "websocket")
            .header("Sec-WebSocket-Version", "13")
            .header("Sec-WebSocket-Key", m_ws_key)
            .build();

        m_send_buffer = request.toString();
        HTTP_LOG_INFO("[wss] [upgrade] [send]");
    }

    bool parseUpgradeResponse() {
        auto read_iovecs = m_upgrader->m_ring_buffer->getReadIovecs();
        if (read_iovecs.empty()) {
            return false;
        }

        auto [error_code, consumed] = m_upgrade_response.fromIOVec(read_iovecs);
        if (consumed > 0) {
            m_upgrader->m_ring_buffer->consume(consumed);
        }

        if (error_code == HttpErrorCode::kIncomplete || error_code == HttpErrorCode::kHeaderInComplete) {
            return false;
        }

        if (error_code != HttpErrorCode::kNoError) {
            setProtocolError("Failed to parse upgrade response");
            return true;
        }

        if (!m_upgrade_response.isComplete()) {
            return false;
        }

        if (!validateUpgradeResponse()) {
            return true;
        }

        *m_upgrader->m_ws_conn_ptr = std::make_unique<WsConnImpl<SocketType>>(
            std::move(*m_upgrader->m_socket),
            std::move(*m_upgrader->m_ring_buffer),
            false
        );

        HTTP_LOG_INFO("[wss] [conn] [ready]");
        m_done = true;
        return true;
    }

    bool validateUpgradeResponse() {
        if (m_upgrade_response.header().code() != HttpStatusCode::SwitchingProtocol_101) {
            m_error = WsError(
                kWsUpgradeFailed,
                "Upgrade failed with status " +
                std::to_string(static_cast<int>(m_upgrade_response.header().code()))
            );
            return false;
        }

        if (!m_upgrade_response.header().headerPairs().hasKey("Sec-WebSocket-Accept")) {
            m_error = WsError(kWsUpgradeFailed, "Missing Sec-WebSocket-Accept header");
            return false;
        }

        std::string accept_key = m_upgrade_response.header().headerPairs().getValue("Sec-WebSocket-Accept");
        std::string expected_accept = WsUpgrade::generateAcceptKey(m_ws_key);
        if (accept_key != expected_accept) {
            m_error = WsError(kWsUpgradeFailed, "Invalid Sec-WebSocket-Accept value");
            return false;
        }

        HTTP_LOG_INFO("[wss] [upgrade] [ok]");
        return true;
    }

    void setSslSendError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            m_error = WsError(kWsConnectionClosed, "Connection closed by peer");
            return;
        }
        m_error = WsError(kWsSendError, error.message());
    }

    void setSslRecvError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            m_error = WsError(kWsConnectionClosed, "Connection closed by peer");
            return;
        }
        m_error = WsError(kWsConnectionError, error.message());
    }

    void setProtocolError(std::string message) {
        m_error = WsError(kWsProtocolError, std::move(message));
    }

    WsUpgraderImpl<SocketType>* m_upgrader;
    std::string m_ws_key;
    std::string m_send_buffer;
    HttpResponse m_upgrade_response;
    size_t m_send_offset = 0;
    bool m_send_completed = false;
    char m_dummy_recv_buffer[1]{0};
    UpgradeSendAwaitable m_send_awaitable;
    UpgradeRecvAwaitable m_recv_awaitable;
    std::optional<WsError> m_error;
    bool m_done = false;

public:
    std::expected<bool, galay::kernel::IOError> m_result;
};

/**
 * @brief WSS (WebSocket Secure) 客户端配置
 */
struct WssClientConfig
{
    // SSL 配置
    std::string ca_path;            // CA 证书路径（可选，用于验证服务器）
    bool verify_peer = false;       // 是否验证服务器证书
    int verify_depth = 4;           // 证书链验证深度
    HeaderPair::NormalizeMode header_mode = HeaderPair::NormalizeMode::Canonical;
};

class WssClient;

class WssClientBuilder {
public:
    WssClientBuilder& caPath(std::string v)              { m_config.ca_path = std::move(v); return *this; }
    WssClientBuilder& verifyPeer(bool v)                 { m_config.verify_peer = v; return *this; }
    WssClientBuilder& verifyDepth(int v)                 { m_config.verify_depth = v; return *this; }
    WssClientBuilder& headerMode(HeaderPair::NormalizeMode v) { m_config.header_mode = v; return *this; }
    WssClient build() const;
    WssClientConfig buildConfig() const                  { return m_config; }
private:
    WssClientConfig m_config;
};

/**
 * @brief WSS (WebSocket Secure) 客户端类
 * @details 基于 SslSocket 的 WebSocket 客户端，支持 wss:// 协议
 */
class WssClient : public WsClientImpl<galay::ssl::SslSocket>
{
public:
    WssClient(const WssClientConfig& config = WssClientConfig())
        : WsClientImpl<galay::ssl::SslSocket>()
        , m_wss_config(config)
        , m_ssl_ctx(galay::ssl::SslMethod::TLS_Client)
    {
        initSslContext();
    }

    ~WssClient() = default;

    WssClient(const WssClient&) = delete;
    WssClient& operator=(const WssClient&) = delete;
    WssClient(WssClient&&) = delete;
    WssClient& operator=(WssClient&&) = delete;

    auto connect(const std::string& url) {
        auto parsed_url = WsUrl::parse(url);
        if (!parsed_url) {
            throw std::runtime_error("Invalid WebSocket URL: " + url);
        }

        m_url = parsed_url.value();

        if (!m_url.is_secure) {
            HTTP_LOG_WARN("[wss] [upgrade] [forced]");
        }

        HTTP_LOG_INFO("[connect] [wss] [{}:{}{}]", m_url.host, m_url.port, m_url.path);

        // 创建 SslSocket
        m_socket = std::make_unique<galay::ssl::SslSocket>(&m_ssl_ctx);

        auto nonblock_result = m_socket->option().handleNonBlock();
        if (!nonblock_result) {
            throw std::runtime_error("Failed to set non-blocking: " + nonblock_result.error().message());
        }

        // 设置 SNI (Server Name Indication)
        auto sni_result = m_socket->setHostname(m_url.host);
        if (!sni_result) {
            HTTP_LOG_WARN("[sni] [fail] [{}]", sni_result.error().message());
        }

        Host server_host(IPType::IPV4, m_url.host, m_url.port);
        return m_socket->connect(server_host);
    }

    /**
     * @brief 执行 SSL 握手（协议完成后再唤醒）
     */
    auto handshake() {
        if (!m_socket) {
            throw std::runtime_error("WssClient not connected. Call connect() first.");
        }
        return m_socket->handshake();
    }

    /**
     * @brief 检查 SSL 握手是否完成
     */
    bool isHandshakeCompleted() const {
        if (!m_socket) return false;
        return m_socket->isHandshakeCompleted();
    }

private:
    void initSslContext() {
        if (!m_ssl_ctx.isValid()) {
            throw std::runtime_error("Failed to create SSL context");
        }

        // 加载 CA 证书
        if (!m_wss_config.ca_path.empty()) {
            auto result = m_ssl_ctx.loadCACertificate(m_wss_config.ca_path);
            if (!result) {
                HTTP_LOG_WARN("[ssl] [ca] [load-fail] [{}]", m_wss_config.ca_path);
            }
        }

        // 设置验证模式
        if (m_wss_config.verify_peer) {
            m_ssl_ctx.setVerifyMode(galay::ssl::SslVerifyMode::Peer);
            m_ssl_ctx.setVerifyDepth(m_wss_config.verify_depth);
        } else {
            m_ssl_ctx.setVerifyMode(galay::ssl::SslVerifyMode::None);
        }
    }

    WssClientConfig m_wss_config;
    galay::ssl::SslContext m_ssl_ctx;
};

// 类型别名 - WebSocket over SSL
using WssUpgrader = WsUpgraderImpl<galay::ssl::SslSocket>;
inline WssClient WssClientBuilder::build() const { return WssClient(m_config); }

} // namespace galay::websocket
#endif

#endif // GALAY_WS_CLIENT_H
