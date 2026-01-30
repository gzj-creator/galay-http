#ifndef GALAY_WS_CLIENT_H
#define GALAY_WS_CLIENT_H

#include "WsConn.h"
#include "WsReader.h"
#include "WsWriter.h"
#include "WsUpgrade.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/kernel/http/HttpLog.h"
#include "galay-http/utils/Http1_1RequestBuilder.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/common/Buffer.h"
#include "galay-kernel/kernel/Awaitable.h"
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
            HTTP_LOG_ERROR("Invalid WebSocket URL format: {}", url);
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
                HTTP_LOG_ERROR("Invalid port number in URL: {}", url);
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

/**
 * @brief WebSocket 客户端升级等待体模板类
 */
template<typename SocketType>
class WsClientUpgradeAwaitableImpl
{
public:
    using SendAwaitableType = decltype(std::declval<SocketType>().send(std::declval<const char*>(), std::declval<size_t>()));
    using ReadvAwaitableType = decltype(std::declval<SocketType>().readv(std::declval<std::vector<iovec>>()));

    WsClientUpgradeAwaitableImpl(WsClientImpl<SocketType>* client)
        : m_client(client)
        , m_state(State::Invalid)
        , m_send_offset(0)
    {
    }

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle) {
        if (m_state == State::Invalid) {
            m_state = State::Sending;

            m_ws_key = generateWebSocketKey();

            m_upgrade_request = Http1_1RequestBuilder::get(m_client->m_url.path)
                .host(m_client->m_url.host + ":" + std::to_string(m_client->m_url.port))
                .header("Connection", "Upgrade")
                .header("Upgrade", "websocket")
                .header("Sec-WebSocket-Version", "13")
                .header("Sec-WebSocket-Key", m_ws_key)
                .build();

            m_send_buffer = m_upgrade_request.toString();
            m_send_offset = 0;

            HTTP_LOG_INFO("Sending WebSocket upgrade request...");

            size_t remaining = m_send_buffer.size() - m_send_offset;
            const char* send_ptr = m_send_buffer.data() + m_send_offset;
            m_send_awaitable.emplace(m_client->m_socket->send(send_ptr, remaining));
            return m_send_awaitable->await_suspend(handle);

        } else if (m_state == State::Sending) {
            size_t remaining = m_send_buffer.size() - m_send_offset;
            const char* send_ptr = m_send_buffer.data() + m_send_offset;
            m_send_awaitable.emplace(m_client->m_socket->send(send_ptr, remaining));
            return m_send_awaitable->await_suspend(handle);

        } else {
            m_recv_awaitable.emplace(m_client->m_socket->readv(m_client->m_ring_buffer->getWriteIovecs()));
            return m_recv_awaitable->await_suspend(handle);
        }
    }

    std::expected<bool, WsError> await_resume() {
        if (m_state == State::Sending) {
            auto send_result = m_send_awaitable->await_resume();

            if (!send_result) {
                HTTP_LOG_ERROR("Failed to send upgrade request: {}", send_result.error().message());
                reset();
                return std::unexpected(WsError(kWsConnectionError,
                    "Failed to send upgrade request: " + send_result.error().message()));
            }

            m_send_offset += send_result.value();

            if (m_send_offset < m_send_buffer.size()) {
                return false;
            }

            HTTP_LOG_INFO("Upgrade request sent, waiting for response...");
            m_state = State::Receiving;
            m_send_awaitable.reset();
            return false;

        } else if (m_state == State::Receiving) {
            auto recv_result = m_recv_awaitable->await_resume();

            if (!recv_result) {
                HTTP_LOG_ERROR("Failed to receive upgrade response: {}", recv_result.error().message());
                reset();
                return std::unexpected(WsError(kWsConnectionError,
                    "Failed to receive upgrade response: " + recv_result.error().message()));
            }

            m_client->m_ring_buffer->produce(recv_result.value());

            auto parse_result = m_upgrade_response.fromIOVec(
                m_client->m_ring_buffer->getReadIovecs());

            if (parse_result.first != HttpErrorCode::kNoError) {
                HTTP_LOG_ERROR("Failed to parse upgrade response: error code {}",
                              static_cast<int>(parse_result.first));
                reset();
                return std::unexpected(WsError(kWsProtocolError,
                    "Failed to parse upgrade response"));
            }

            if (!m_upgrade_response.isComplete()) {
                return false;
            }

            HTTP_LOG_INFO("Received complete upgrade response");

            if (m_upgrade_response.header().code() != HttpStatusCode::SwitchingProtocol_101) {
                HTTP_LOG_ERROR("WebSocket upgrade failed. Status: {} {}",
                              static_cast<int>(m_upgrade_response.header().code()),
                              httpStatusCodeToString(m_upgrade_response.header().code()));
                reset();
                return std::unexpected(WsError(kWsUpgradeFailed,
                    "Upgrade failed with status " +
                    std::to_string(static_cast<int>(m_upgrade_response.header().code()))));
            }

            if (!m_upgrade_response.header().headerPairs().hasKey("Sec-WebSocket-Accept")) {
                HTTP_LOG_ERROR("Missing Sec-WebSocket-Accept header in response");
                reset();
                return std::unexpected(WsError(kWsUpgradeFailed,
                    "Missing Sec-WebSocket-Accept header"));
            }

            std::string accept_key = m_upgrade_response.header().headerPairs()
                .getValue("Sec-WebSocket-Accept");
            std::string expected_accept = WsUpgrade::generateAcceptKey(m_ws_key);

            if (accept_key != expected_accept) {
                HTTP_LOG_ERROR("Invalid Sec-WebSocket-Accept value");
                reset();
                return std::unexpected(WsError(kWsUpgradeFailed,
                    "Invalid Sec-WebSocket-Accept value"));
            }

            HTTP_LOG_INFO("WebSocket upgrade successful!");

            size_t consumed = parse_result.second;
            m_client->m_ring_buffer->consume(consumed);

            m_client->m_ws_conn = std::make_unique<WsConnImpl<SocketType>>(
                std::move(*m_client->m_socket),
                std::move(*m_client->m_ring_buffer),
                m_client->m_reader_setting,
                m_client->m_writer_setting,
                false
            );

            HTTP_LOG_INFO("WsConn created successfully");

            m_client->m_socket.reset();
            m_client->m_ring_buffer.reset();
            m_client->m_upgrade_awaitable.reset();

            return true;

        } else {
            HTTP_LOG_ERROR("await_resume called in Invalid state");
            reset();
            return std::unexpected(WsError(kWsProtocolError, "Invalid state"));
        }
    }

    bool isInvalid() const {
        return m_state == State::Invalid;
    }

    void reset() {
        m_state = State::Invalid;
        m_send_awaitable.reset();
        m_recv_awaitable.reset();
        m_send_buffer.clear();
        m_send_offset = 0;
    }

private:
    enum class State {
        Invalid,
        Sending,
        Receiving
    };

    WsClientImpl<SocketType>* m_client;
    State m_state;

    std::optional<SendAwaitableType> m_send_awaitable;
    std::optional<ReadvAwaitableType> m_recv_awaitable;

    std::string m_send_buffer;
    size_t m_send_offset;

    std::string m_ws_key;
    HttpRequest m_upgrade_request;
    HttpResponse m_upgrade_response;
};

/**
 * @brief WebSocket 客户端模板类
 */
template<typename SocketType>
class WsClientImpl
{
public:
    WsClientImpl(const WsReaderSetting& reader_setting = WsReaderSetting(),
                 const WsWriterSetting& writer_setting = WsWriterSetting(),
                 size_t ring_buffer_size = 8192)
        : m_reader_setting(reader_setting)
        , m_writer_setting(writer_setting)
        , m_ring_buffer_size(ring_buffer_size)
        , m_socket(nullptr)
        , m_ring_buffer(nullptr)
        , m_ws_conn(nullptr)
        , m_upgrade_awaitable(nullptr)
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

        HTTP_LOG_INFO("Connecting to WebSocket server at {}:{}{}",
                     m_url.host, m_url.port, m_url.path);

        m_socket = std::make_unique<SocketType>(IPType::IPV4);
        m_ring_buffer = std::make_unique<RingBuffer>(m_ring_buffer_size);

        auto nonblock_result = m_socket->option().handleNonBlock();
        if (!nonblock_result) {
            throw std::runtime_error("Failed to set non-blocking: " + nonblock_result.error().message());
        }

        Host server_host(IPType::IPV4, m_url.host, m_url.port);
        return m_socket->connect(server_host);
    }

    WsClientUpgradeAwaitableImpl<SocketType>& upgrade() {
        if (!m_socket) {
            throw std::runtime_error("WsClient not connected. Call connect() first.");
        }

        if (!m_upgrade_awaitable || m_upgrade_awaitable->isInvalid()) {
            m_upgrade_awaitable = std::make_unique<WsClientUpgradeAwaitableImpl<SocketType>>(this);
        }

        return *m_upgrade_awaitable;
    }

    bool isConnected() const {
        return m_ws_conn != nullptr;
    }

    auto close() {
        if (!m_ws_conn) {
            throw std::runtime_error("WsClient not connected");
        }
        return m_ws_conn->close();
    }

    void setReaderSetting(const WsReaderSetting& setting) {
        m_reader_setting = setting;
    }

    void setWriterSetting(const WsWriterSetting& setting) {
        m_writer_setting = setting;
    }

    WsReaderImpl<SocketType>& getWsReader() {
        return m_ws_conn->getReader();
    }

    WsWriterImpl<SocketType>& getWsWriter() {
        return m_ws_conn->getWriter();
    }

    WsConnImpl<SocketType>* getConn() {
        return m_ws_conn.get();
    }

    friend class WsClientUpgradeAwaitableImpl<SocketType>;

private:
    WsReaderSetting m_reader_setting;
    WsWriterSetting m_writer_setting;
    size_t m_ring_buffer_size;

    std::unique_ptr<SocketType> m_socket;
    std::unique_ptr<RingBuffer> m_ring_buffer;

    std::unique_ptr<WsConnImpl<SocketType>> m_ws_conn;

    WsUrl m_url;

    std::unique_ptr<WsClientUpgradeAwaitableImpl<SocketType>> m_upgrade_awaitable;
};

// 类型别名 - WebSocket over TCP
using WsClientUpgradeAwaitable = WsClientUpgradeAwaitableImpl<TcpSocket>;
using WsClient = WsClientImpl<TcpSocket>;

} // namespace galay::websocket

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/SslSocket.h"
namespace galay::websocket {
using WssClientUpgradeAwaitable = WsClientUpgradeAwaitableImpl<galay::ssl::SslSocket>;
using WssClient = WsClientImpl<galay::ssl::SslSocket>;
} // namespace galay::websocket
#endif

#endif // GALAY_WS_CLIENT_H
