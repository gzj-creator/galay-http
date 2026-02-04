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
#include <string>
#include <optional>
#include <expected>
#include <memory>
#include <regex>
#include <random>

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

// 前向声明
template<typename SocketType>
class WsClientImpl;

template<typename SocketType>
class WsUpgraderImpl;

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
 * while (true) {
 *     auto result = co_await upgrader();
 *     if (!result) {
 *         // 处理错误
 *         break;
 *     }
 *     if (result.value()) {
 *         // 升级完成
 *         break;
 *     }
 *     // 继续升级
 * }
 * @endcode
 */
template<typename SocketType>
class WsUpgraderImpl
{
public:
    using SendAwaitableType = decltype(std::declval<SocketType>().send(std::declval<const char*>(), std::declval<size_t>()));
    using ReadvAwaitableType = decltype(std::declval<SocketType>().readv(std::declval<std::vector<iovec>>()));

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
        , m_state(State::Invalid)
        , m_send_offset(0)
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
    enum class State {
        Invalid,
        Sending,
        Receiving
    };

    // 引用 WsClient 的资源（不拥有所有权）
    SocketType* m_socket;
    RingBuffer* m_ring_buffer;
    const WsUrl& m_url;
    const WsReaderSetting& m_reader_setting;
    const WsWriterSetting& m_writer_setting;
    std::unique_ptr<WsConnImpl<SocketType>>* m_ws_conn_ptr;

    // 升级过程的状态
    State m_state;

    std::optional<SendAwaitableType> m_send_awaitable;
    std::optional<ReadvAwaitableType> m_recv_awaitable;

    std::string m_send_buffer;
    size_t m_send_offset;

    std::string m_ws_key;
    HttpRequest m_upgrade_request;
    HttpResponse m_upgrade_response;
};

// WsUpgradeAwaitableImpl 的实现
template<typename SocketType>
bool WsUpgradeAwaitableImpl<SocketType>::await_suspend(std::coroutine_handle<> handle) {
    auto& upgrader = *m_upgrader;

    if (upgrader.m_state == WsUpgraderImpl<SocketType>::State::Invalid) {
        upgrader.m_state = WsUpgraderImpl<SocketType>::State::Sending;

        upgrader.m_ws_key = generateWebSocketKey();

        upgrader.m_upgrade_request = Http1_1RequestBuilder::get(upgrader.m_url.path)
            .host(upgrader.m_url.host + ":" + std::to_string(upgrader.m_url.port))
            .header("Connection", "Upgrade")
            .header("Upgrade", "websocket")
            .header("Sec-WebSocket-Version", "13")
            .header("Sec-WebSocket-Key", upgrader.m_ws_key)
            .build();

        upgrader.m_send_buffer = upgrader.m_upgrade_request.toString();
        upgrader.m_send_offset = 0;

        HTTP_LOG_INFO("[ws] [upgrade] [send]");

        size_t remaining = upgrader.m_send_buffer.size() - upgrader.m_send_offset;
        const char* send_ptr = upgrader.m_send_buffer.data() + upgrader.m_send_offset;
        upgrader.m_send_awaitable.emplace(upgrader.m_socket->send(send_ptr, remaining));
        return upgrader.m_send_awaitable->await_suspend(handle);

    } else if (upgrader.m_state == WsUpgraderImpl<SocketType>::State::Sending) {
        size_t remaining = upgrader.m_send_buffer.size() - upgrader.m_send_offset;
        const char* send_ptr = upgrader.m_send_buffer.data() + upgrader.m_send_offset;
        upgrader.m_send_awaitable.emplace(upgrader.m_socket->send(send_ptr, remaining));
        return upgrader.m_send_awaitable->await_suspend(handle);

    } else {
        upgrader.m_recv_awaitable.emplace(upgrader.m_socket->readv(upgrader.m_ring_buffer->getWriteIovecs()));
        return upgrader.m_recv_awaitable->await_suspend(handle);
    }
}

template<typename SocketType>
std::expected<bool, WsError> WsUpgradeAwaitableImpl<SocketType>::await_resume() {
    auto& upgrader = *m_upgrader;

    if (upgrader.m_state == WsUpgraderImpl<SocketType>::State::Sending) {
        auto send_result = upgrader.m_send_awaitable->await_resume();

        if (!send_result) {
            HTTP_LOG_ERROR("[ws] [upgrade] [send-fail] [{}]", send_result.error().message());
            return std::unexpected(WsError(kWsConnectionError,
                "Failed to send upgrade request: " + send_result.error().message()));
        }

        upgrader.m_send_offset += send_result.value();

        if (upgrader.m_send_offset < upgrader.m_send_buffer.size()) {
            return false;
        }

        HTTP_LOG_INFO("[ws] [upgrade] [wait]");
        upgrader.m_state = WsUpgraderImpl<SocketType>::State::Receiving;
        upgrader.m_send_awaitable.reset();
        return false;

    } else if (upgrader.m_state == WsUpgraderImpl<SocketType>::State::Receiving) {
        auto recv_result = upgrader.m_recv_awaitable->await_resume();

        if (!recv_result) {
            HTTP_LOG_ERROR("[ws] [upgrade] [recv-fail] [{}]", recv_result.error().message());
            return std::unexpected(WsError(kWsConnectionError,
                "Failed to receive upgrade response: " + recv_result.error().message()));
        }

        upgrader.m_ring_buffer->produce(recv_result.value());

        auto parse_result = upgrader.m_upgrade_response.fromIOVec(
            upgrader.m_ring_buffer->getReadIovecs());

        if (parse_result.first != HttpErrorCode::kNoError) {
            HTTP_LOG_ERROR("[ws] [upgrade] [parse-fail] [code={}]",
                          static_cast<int>(parse_result.first));
            return std::unexpected(WsError(kWsProtocolError,
                "Failed to parse upgrade response"));
        }

        if (!upgrader.m_upgrade_response.isComplete()) {
            return false;
        }

        HTTP_LOG_INFO("[ws] [upgrade] [recv-ok]");

        if (upgrader.m_upgrade_response.header().code() != HttpStatusCode::SwitchingProtocol_101) {
            HTTP_LOG_ERROR("[ws] [upgrade] [fail] [{}] [{}]",
                          static_cast<int>(upgrader.m_upgrade_response.header().code()),
                          httpStatusCodeToString(upgrader.m_upgrade_response.header().code()));
            return std::unexpected(WsError(kWsUpgradeFailed,
                "Upgrade failed with status " +
                std::to_string(static_cast<int>(upgrader.m_upgrade_response.header().code()))));
        }

        if (!upgrader.m_upgrade_response.header().headerPairs().hasKey("Sec-WebSocket-Accept")) {
            HTTP_LOG_ERROR("[ws] [upgrade] [accept-missing]");
            return std::unexpected(WsError(kWsUpgradeFailed,
                "Missing Sec-WebSocket-Accept header"));
        }

        std::string accept_key = upgrader.m_upgrade_response.header().headerPairs()
            .getValue("Sec-WebSocket-Accept");
        std::string expected_accept = WsUpgrade::generateAcceptKey(upgrader.m_ws_key);

        if (accept_key != expected_accept) {
            HTTP_LOG_ERROR("[ws] [upgrade] [accept-invalid]");
            return std::unexpected(WsError(kWsUpgradeFailed,
                "Invalid Sec-WebSocket-Accept value"));
        }

        HTTP_LOG_INFO("[ws] [upgrade] [ok]");

        size_t consumed = parse_result.second;
        upgrader.m_ring_buffer->consume(consumed);

        // 创建 WsConn，转移 socket 和 ring_buffer 的所有权
        *upgrader.m_ws_conn_ptr = std::make_unique<WsConnImpl<SocketType>>(
            std::move(*upgrader.m_socket),
            std::move(*upgrader.m_ring_buffer),
            upgrader.m_reader_setting,
            upgrader.m_writer_setting,
            false
        );

        HTTP_LOG_INFO("[ws] [conn] [ready]");

        return true;

    } else {
        HTTP_LOG_ERROR("[state] [invalid] [await-resume]");
        return std::unexpected(WsError(kWsProtocolError, "Invalid state"));
    }
}

/**
 * @brief WebSocket 客户端模板类
 * @details 只负责连接，通过getSession()获取Session进行通信
 */
template<typename SocketType>
class WsClientImpl
{
public:
    WsClientImpl()
        : m_socket(nullptr)
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

private:
    std::unique_ptr<SocketType> m_socket;
    WsUrl m_url;
};

// 类型别名 - WebSocket over TCP
using WsUpgrader = WsUpgraderImpl<TcpSocket>;
using WsClient = WsClientImpl<TcpSocket>;

} // namespace galay::websocket

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/SslSocket.h"

namespace galay::websocket {

/**
 * @note WSS 客户端说明
 *
 * 由于 galay::ssl::SslSocket 目前不支持 readv 方法，WsClientImpl 模板
 * 不能直接用于 SslSocket。
 *
 * 要实现 WSS 客户端，请参考 example/E8-WssClient.cc 示例，
 * 直接使用 SslSocket 和 WebSocketFrame 进行操作：
 *
 * 1. 创建 SslContext 和 SslSocket
 * 2. 执行 TCP 连接 (socket.connect())
 * 3. 执行 SSL 握手 (socket.handshake())
 * 4. 发送 WebSocket 升级请求
 * 5. 接收并验证升级响应
 * 6. 使用 WsFrameParser 进行帧的编解码
 *
 * 完整的 WssClient 类支持需要为 SslSocket 添加 readv 方法。
 */

} // namespace galay::websocket
#endif

#endif // GALAY_WS_CLIENT_H
