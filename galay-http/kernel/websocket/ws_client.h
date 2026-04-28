#ifndef GALAY_WS_CLIENT_H
#define GALAY_WS_CLIENT_H

#include "ws_session.h"
#include "ws_url.h"
#include "ws_conn.h"
#include "ws_upgrade.h"
#include "galay-http/kernel/iov_utils.h"
#include "galay-http/kernel/http/http_log.h"
#include "galay-http/protoc/http/http_header.h"
#include "galay-http/protoc/http/http_request.h"
#include "galay-http/protoc/http/http_response.h"
#include "galay-http/utils/req_bld.h"
#include "galay-kernel/async/tcp_socket.h"
#include "galay-kernel/common/buffer.h"
#include "galay-kernel/kernel/awaitable.h"
#include <array>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/async/ssl_awaitable_core.h"
#include "galay-ssl/async/ssl_socket.h"
#include "galay-ssl/ssl/ssl_context.h"
#endif

namespace galay::websocket
{

using namespace galay::async;
using namespace galay::kernel;
using namespace galay::http;

template<typename SocketType>
class WsClientImpl;

template<typename SocketType>
class WsUpgraderImpl;

struct WsClientConfig
{
    HeaderPair::Mode header_mode = HeaderPair::Mode::ClientSide;
};

/**
 * @brief WebSocket 客户端 builder
 * @details builder 只收集升级请求所需的头部策略，不直接持有网络资源。
 */
class WsClientBuilder {
public:
    WsClientBuilder& headerMode(HeaderPair::Mode v) { m_config.header_mode = v; return *this; }
    WsClientImpl<TcpSocket> build() const;
    WsClientConfig buildConfig() const { return m_config; }

private:
    WsClientConfig m_config;
};

namespace detail {

template<typename SocketType>
struct WsClientUpgradeState {
    using ResultType = std::expected<bool, WsError>;

    WsClientUpgradeState(SocketType* socket,
                         RingBuffer* ring_buffer,
                         WsUrl url,
                         std::unique_ptr<WsConnImpl<SocketType>>* ws_conn_ptr)
        : m_socket(socket)
        , m_ring_buffer(ring_buffer)
        , m_url(std::move(url))
        , m_ws_conn_ptr(ws_conn_ptr)
    {
        initialize();
    }

    bool isFinished() const {
        return m_result.has_value() || m_error.has_value();
    }

    ResultType takeResult() {
        if (m_error.has_value()) {
            return std::unexpected(std::move(*m_error));
        }
        return m_result.value_or(ResultType(true));
    }

    bool hasPendingSend() const {
        return !isFinished() && m_send_offset < m_send_buffer.size();
    }

    const char* sendData() const {
        return m_send_buffer.data() + m_send_offset;
    }

    size_t remainingSendBytes() const {
        return m_send_buffer.size() - m_send_offset;
    }

    void prepareSendWindow() {
        if (!hasPendingSend()) {
            m_send_iovecs[0] = {.iov_base = nullptr, .iov_len = 0};
            return;
        }

        m_send_iovecs[0] = {
            .iov_base = const_cast<char*>(sendData()),
            .iov_len = remainingSendBytes()
        };
    }

    const struct iovec* sendIovecsData() const {
        return m_send_iovecs.data();
    }

    size_t sendIovecsCount() const {
        return hasPendingSend() ? 1 : 0;
    }

    void onBytesSent(size_t sent_bytes) {
        if (sent_bytes == 0) {
            return;
        }
        if (sent_bytes > remainingSendBytes()) {
            setProtocolError("Send progress overflow");
            return;
        }
        m_send_offset += sent_bytes;
    }

    bool prepareRecvWindow() {
        if (!ensureResources()) {
            return false;
        }
        m_write_iovecs = borrowWriteIovecs(*m_ring_buffer);
        return !m_write_iovecs.empty();
    }

    bool prepareRecvWindow(char*& buffer, size_t& length) {
        if (!prepareRecvWindow()) {
            buffer = nullptr;
            length = 0;
            return false;
        }

        if (!IoVecWindow::bindFirstNonEmpty(m_write_iovecs, buffer, length)) {
            buffer = nullptr;
            length = 0;
            return false;
        }

        return length > 0;
    }

    const struct iovec* recvIovecsData() const {
        return m_write_iovecs.data();
    }

    size_t recvIovecsCount() const {
        return m_write_iovecs.size();
    }

    void onBytesReceived(size_t recv_bytes) {
        if (ensureResources()) {
            m_ring_buffer->produce(recv_bytes);
        }
    }

    bool tryParseUpgradeResponse() {
        if (!ensureResources()) {
            return true;
        }

        auto read_iovecs = borrowReadIovecs(*m_ring_buffer);
        if (read_iovecs.empty()) {
            return false;
        }

        std::vector<iovec> parse_iovecs;
        if (IoVecWindow::buildWindow(read_iovecs, parse_iovecs) == 0) {
            return false;
        }

        auto [error_code, consumed] = m_upgrade_response.fromIOVec(parse_iovecs);
        if (consumed > 0) {
            m_ring_buffer->consume(consumed);
        }

        if (error_code == HttpErrorCode::kIncomplete ||
            error_code == HttpErrorCode::kHeaderInComplete) {
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

        *m_ws_conn_ptr = std::make_unique<WsConnImpl<SocketType>>(
            std::move(*m_socket),
            std::move(*m_ring_buffer),
            false);
        HTTP_LOG_INFO("[{}] [conn] [ready]", logScheme());
        m_result = true;
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

#ifdef GALAY_HTTP_SSL_ENABLED
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
#endif

    void setProtocolError(std::string message) {
        m_error = WsError(kWsProtocolError, std::move(message));
    }

private:
    void initialize() {
        if (!ensureResources()) {
            return;
        }

        if (*m_ws_conn_ptr != nullptr) {
            m_result = true;
            return;
        }

        m_ws_key = generateWebSocketKey();
        auto request = Http1_1RequestBuilder::get(m_url.path)
            .host(m_url.host + ":" + std::to_string(m_url.port))
            .header("Connection", "Upgrade")
            .header("Upgrade", "websocket")
            .header("Sec-WebSocket-Version", "13")
            .header("Sec-WebSocket-Key", m_ws_key)
            .build();
        m_send_buffer = request.toString();
        HTTP_LOG_INFO("[{}] [upgrade] [send]", logScheme());
    }

    bool validateUpgradeResponse() {
        if (m_upgrade_response.header().code() != HttpStatusCode::SwitchingProtocol_101) {
            return setUpgradeFailed(
                "Upgrade failed with status " +
                std::to_string(static_cast<int>(m_upgrade_response.header().code())));
        }

        if (!m_upgrade_response.header().headerPairs().hasKey("Sec-WebSocket-Accept")) {
            return setUpgradeFailed("Missing Sec-WebSocket-Accept header");
        }

        const std::string accept_key =
            m_upgrade_response.header().headerPairs().getValue("Sec-WebSocket-Accept");
        if (accept_key != WsUpgrade::generateAcceptKey(m_ws_key)) {
            return setUpgradeFailed("Invalid Sec-WebSocket-Accept value");
        }

        HTTP_LOG_INFO("[{}] [upgrade] [ok]", logScheme());
        return true;
    }

    bool setUpgradeFailed(std::string message) {
        m_error = WsError(kWsUpgradeFailed, std::move(message));
        return false;
    }

    bool ensureResources() {
        if (m_socket != nullptr && m_ring_buffer != nullptr && m_ws_conn_ptr != nullptr) {
            return true;
        }

        if (!m_error.has_value()) {
            m_error = WsError(kWsConnectionError, "WsClient not connected. Call connect() first.");
        }
        return false;
    }

    const char* logScheme() const {
        if constexpr (std::is_same_v<SocketType, TcpSocket>) {
            return "ws";
        } else {
            return "wss";
        }
    }

    SocketType* m_socket = nullptr;
    RingBuffer* m_ring_buffer = nullptr;
    WsUrl m_url;
    std::unique_ptr<WsConnImpl<SocketType>>* m_ws_conn_ptr = nullptr;
    std::string m_ws_key;
    std::string m_send_buffer;
    size_t m_send_offset = 0;
    HttpResponse m_upgrade_response;
    std::array<struct iovec, 1> m_send_iovecs{};
    BorrowedIovecs<2> m_write_iovecs;
    std::optional<ResultType> m_result;
    std::optional<WsError> m_error;
};

template<typename StateT>
struct WsClientTcpUpgradeMachine {
    using result_type = typename StateT::ResultType;

    explicit WsClientTcpUpgradeMachine(std::shared_ptr<StateT> state)
        : m_state(std::move(state)) {}

    MachineAction<result_type> advance() {
        if (m_state->isFinished()) {
            return MachineAction<result_type>::complete(m_state->takeResult());
        }

        if (m_state->hasPendingSend()) {
            m_state->prepareSendWindow();
            return MachineAction<result_type>::waitWritev(
                m_state->sendIovecsData(),
                m_state->sendIovecsCount());
        }

        if (m_state->tryParseUpgradeResponse()) {
            return MachineAction<result_type>::complete(m_state->takeResult());
        }

        if (!m_state->prepareRecvWindow()) {
            if (!m_state->isFinished()) {
                m_state->setProtocolError("Upgrade response too large");
            }
            return MachineAction<result_type>::complete(m_state->takeResult());
        }

        return MachineAction<result_type>::waitReadv(
            m_state->recvIovecsData(),
            m_state->recvIovecsCount());
    }

    void onRead(std::expected<size_t, IOError> result) {
        if (!result) {
            m_state->setRecvError(result.error());
            return;
        }

        if (result.value() == 0) {
            m_state->setProtocolError("Connection closed");
            return;
        }

        m_state->onBytesReceived(result.value());
    }

    void onWrite(std::expected<size_t, IOError> result) {
        if (!result) {
            m_state->setSendError(result.error());
            return;
        }

        m_state->onBytesSent(result.value());
    }

    std::shared_ptr<StateT> m_state;
};

#ifdef GALAY_HTTP_SSL_ENABLED
template<typename StateT>
struct WsClientSslUpgradeMachine {
    using result_type = typename StateT::ResultType;

    explicit WsClientSslUpgradeMachine(std::shared_ptr<StateT> state)
        : m_state(std::move(state)) {}

    galay::ssl::SslMachineAction<result_type> advance() {
        if (m_state->isFinished()) {
            return galay::ssl::SslMachineAction<result_type>::complete(m_state->takeResult());
        }

        if (m_state->hasPendingSend()) {
            return galay::ssl::SslMachineAction<result_type>::send(
                m_state->sendData(),
                m_state->remainingSendBytes());
        }

        if (m_state->tryParseUpgradeResponse()) {
            return galay::ssl::SslMachineAction<result_type>::complete(m_state->takeResult());
        }

        char* recv_buffer = nullptr;
        size_t recv_length = 0;
        if (!m_state->prepareRecvWindow(recv_buffer, recv_length)) {
            if (!m_state->isFinished()) {
                m_state->setProtocolError("Upgrade response too large");
            }
            return galay::ssl::SslMachineAction<result_type>::complete(m_state->takeResult());
        }

        return galay::ssl::SslMachineAction<result_type>::recv(recv_buffer, recv_length);
    }

    void onHandshake(std::expected<void, galay::ssl::SslError>) {}

    void onRecv(std::expected<Bytes, galay::ssl::SslError> result) {
        if (!result) {
            m_state->setSslRecvError(result.error());
            return;
        }

        const size_t recv_bytes = result.value().size();
        if (recv_bytes == 0) {
            m_state->setProtocolError("Connection closed");
            return;
        }

        m_state->onBytesReceived(recv_bytes);
    }

    void onSend(std::expected<size_t, galay::ssl::SslError> result) {
        if (!result) {
            m_state->setSslSendError(result.error());
            return;
        }

        m_state->onBytesSent(result.value());
    }

    void onShutdown(std::expected<void, galay::ssl::SslError>) {}

    std::shared_ptr<StateT> m_state;
};
#endif

template<typename SocketType>
auto buildWsClientUpgradeOperation(SocketType* socket,
                                   RingBuffer* ring_buffer,
                                   const WsUrl& url,
                                   std::unique_ptr<WsConnImpl<SocketType>>* ws_conn_ptr) {
    using StateT = WsClientUpgradeState<SocketType>;
    using ResultType = typename StateT::ResultType;

    auto state = std::make_shared<StateT>(socket, ring_buffer, url, ws_conn_ptr);

    if constexpr (std::is_same_v<SocketType, TcpSocket>) {
        IOController* controller = socket != nullptr ? socket->controller() : nullptr;
        if (ws_conn_ptr != nullptr && *ws_conn_ptr != nullptr) {
            controller = (*ws_conn_ptr)->socket().controller();
        }

        return AwaitableBuilder<ResultType>::fromStateMachine(
                   controller,
                   WsClientTcpUpgradeMachine<StateT>(std::move(state)))
            .build();
    } else {
#ifdef GALAY_HTTP_SSL_ENABLED
        auto* active_socket = socket;
        if (ws_conn_ptr != nullptr && *ws_conn_ptr != nullptr) {
            active_socket = &(*ws_conn_ptr)->socket();
        }

        return galay::ssl::SslAwaitableBuilder<ResultType>::fromStateMachine(
                   active_socket->controller(),
                   active_socket,
                   WsClientSslUpgradeMachine<StateT>(std::move(state)))
            .build();
#else
        static_assert(!sizeof(SocketType), "SSL support is disabled");
#endif
    }
}

} // namespace detail

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

    auto operator()() {
        if (m_socket == nullptr || m_ring_buffer == nullptr || m_ws_conn_ptr == nullptr) {
            throw std::runtime_error("WsClient not connected. Call connect() first.");
        }

        return detail::buildWsClientUpgradeOperation(
            m_socket,
            m_ring_buffer,
            m_url,
            m_ws_conn_ptr);
    }

private:
    SocketType* m_socket;
    RingBuffer* m_ring_buffer;
    const WsUrl& m_url;
    const WsReaderSetting& m_reader_setting;
    const WsWriterSetting& m_writer_setting;
    std::unique_ptr<WsConnImpl<SocketType>>* m_ws_conn_ptr;
};

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
    WsClientImpl(WsClientImpl&&) noexcept = default;
    WsClientImpl& operator=(WsClientImpl&&) noexcept = default;

    /**
     * @brief 发起到底层 WebSocket 目标的 TCP 连接
     * @param url 形如 `ws://host[:port][/path]` 的目标地址
     * @return 底层 socket 的 connect awaitable
     * @throws std::runtime_error URL 非法、协议与客户端类型不匹配、或 socket 初始化失败
     * @note 对明文 `WsClient` 而言，若 URL 为 `wss://`，必须改用 `WssClient`
     */
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
     * @brief 创建一个绑定当前 socket 与 URL 的 WebSocket session
     * @param writer_setting Writer 行为配置
     * @param ring_buffer_size Session RingBuffer 大小
     * @param reader_setting Reader 行为配置
     * @return 以值返回的 Session，内部借用当前客户端持有的 socket
     * @throws std::runtime_error 当前客户端尚未连接
     * @details 客户端典型顺序是：`connect()` -> `getSession()` -> `session.upgrade()`
     */
    WsSessionImpl<SocketType> getSession(const WsWriterSetting& writer_setting,
                                         size_t ring_buffer_size = 8192,
                                         const WsReaderSetting& reader_setting = WsReaderSetting()) {
        if (!m_socket) {
            throw std::runtime_error("WsClient not connected. Call connect() first.");
        }
        return WsSessionImpl<SocketType>(*m_socket, m_url, writer_setting, ring_buffer_size, reader_setting);
    }

    /**
     * @brief 关闭底层 socket
     * @return 底层 socket 的 close awaitable
     * @throws std::runtime_error 当前客户端尚未连接
     */
    auto close() {
        if (!m_socket) {
            throw std::runtime_error("WsClient not connected");
        }
        return m_socket->close();
    }

    SocketType* getSocket() {
        return m_socket.get();
    }

    /**
     * @brief 对支持 TLS 的 socket 执行握手
     * @return TLS socket 的 handshake awaitable；对明文 socket 返回其握手入口
     * @throws std::runtime_error 当前客户端尚未连接
     * @note 明文 `WsClient` 一般不需要显式调用；`WssClient` 则应在升级前先完成 TLS 握手
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
     * @brief 检查底层握手是否已经完成
     * @return 未连接返回 false；明文 socket 视为已完成；TLS socket 返回真实握手状态
     */
    bool isHandshakeCompleted() const {
        if (!m_socket) {
            return false;
        }
        if constexpr (requires { m_socket->isHandshakeCompleted(); }) {
            return m_socket->isHandshakeCompleted();
        }
        return true;
    }

    const WsUrl& url() const { return m_url; }

protected:
    std::unique_ptr<SocketType> m_socket;
    WsClientConfig m_config;
    WsUrl m_url;
};

using WsUpgrader = WsUpgraderImpl<TcpSocket>;
using WsClient = WsClientImpl<TcpSocket>;
inline WsClient WsClientBuilder::build() const { return WsClient(m_config); }

#ifdef GALAY_HTTP_SSL_ENABLED
struct WssClientConfig
{
    std::string ca_path;
    bool verify_peer = false;
    int verify_depth = 4;
    HeaderPair::Mode header_mode = HeaderPair::Mode::ClientSide;
};

class WssClient;

/**
 * @brief WSS 客户端 builder
 * @details 用于配置 TLS 验证策略和升级请求头部策略。
 */
class WssClientBuilder {
public:
    WssClientBuilder& caPath(std::string v) { m_config.ca_path = std::move(v); return *this; }
    WssClientBuilder& verifyPeer(bool v) { m_config.verify_peer = v; return *this; }
    WssClientBuilder& verifyDepth(int v) { m_config.verify_depth = v; return *this; }
    WssClientBuilder& headerMode(HeaderPair::Mode v) { m_config.header_mode = v; return *this; }
    WssClient build() const;
    WssClientConfig buildConfig() const { return m_config; }

private:
    WssClientConfig m_config;
};

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
    WssClient(WssClient&&) noexcept = default;
    WssClient& operator=(WssClient&&) noexcept = default;

    /**
     * @brief 解析 WSS URL、初始化 TLS socket 并发起 TCP 连接
     * @param url 形如 `wss://host[:port][/path]` 的目标地址
     * @return TLS socket 的 connect awaitable
     * @throws std::runtime_error URL 非法或 socket 初始化失败
     * @note 该函数不会自动完成 TLS 握手；成功连接后仍应先 `handshake()` 再执行 `session.upgrade()`
     */
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

        m_socket = std::make_unique<galay::ssl::SslSocket>(&m_ssl_ctx);

        auto nonblock_result = m_socket->option().handleNonBlock();
        if (!nonblock_result) {
            throw std::runtime_error("Failed to set non-blocking: " + nonblock_result.error().message());
        }

        auto sni_result = m_socket->setHostname(m_url.host);
        if (!sni_result) {
            HTTP_LOG_WARN("[sni] [fail] [{}]", sni_result.error().message());
        }

        Host server_host(IPType::IPV4, m_url.host, m_url.port);
        return m_socket->connect(server_host);
    }

    auto handshake() {
        if (!m_socket) {
            throw std::runtime_error("WssClient not connected. Call connect() first.");
        }
        return m_socket->handshake();
    }

    bool isHandshakeCompleted() const {
        if (!m_socket) {
            return false;
        }
        return m_socket->isHandshakeCompleted();
    }

private:
    void initSslContext() {
        if (!m_ssl_ctx.isValid()) {
            throw std::runtime_error("Failed to create SSL context");
        }

        if (!m_wss_config.ca_path.empty()) {
            auto result = m_ssl_ctx.loadCACertificate(m_wss_config.ca_path);
            if (!result) {
                HTTP_LOG_WARN("[ssl] [ca] [load-fail] [{}]", m_wss_config.ca_path);
            }
        }

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

using WssUpgrader = WsUpgraderImpl<galay::ssl::SslSocket>;
inline WssClient WssClientBuilder::build() const { return WssClient(m_config); }
#endif

} // namespace galay::websocket

#endif // GALAY_WS_CLIENT_H
