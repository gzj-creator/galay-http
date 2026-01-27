#include "WsClient.h"
#include "galay-http/utils/Http1_1RequestBuilder.h"
#include "galay-http/kernel/http/HttpLog.h"
#include "galay-http/kernel/http/HttpConn.h"
#include <galay-utils/algorithm/Base64.hpp>
#include "WsUpgrade.h"
#include <random>
#include <regex>

namespace galay::websocket
{

using namespace galay::utils;

// ==================== WsUrl 实现 ====================

std::optional<WsUrl> WsUrl::parse(const std::string& url) {
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

// ==================== 辅助函数 ====================

static std::string generateWebSocketKey() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);

    unsigned char random_bytes[16];
    for (int i = 0; i < 16; i++) {
        random_bytes[i] = static_cast<unsigned char>(dis(gen));
    }

    return Base64Util::Base64Encode(random_bytes, 16);
}

// ==================== WsClient 实现 ====================

WsClient::WsClient(const WsReaderSetting& reader_setting,
                   const WsWriterSetting& writer_setting,
                   size_t ring_buffer_size)
    : m_reader_setting(reader_setting)
    , m_writer_setting(writer_setting)
    , m_ring_buffer_size(ring_buffer_size)
    , m_socket(nullptr)
    , m_ring_buffer(nullptr)
    , m_ws_conn(nullptr)
    , m_upgrade_awaitable(nullptr)
{
}

ConnectAwaitable WsClient::connect(const std::string& url) {
    auto parsed_url = WsUrl::parse(url);
    if (!parsed_url) {
        throw std::runtime_error("Invalid WebSocket URL: " + url);
    }

    m_url = parsed_url.value();

    if (m_url.is_secure) {
        throw std::runtime_error("WSS (secure WebSocket) is not yet supported");
    }

    HTTP_LOG_INFO("Connecting to WebSocket server at {}:{}{}",
                 m_url.host, m_url.port, m_url.path);

    // 创建 socket 和 ring buffer
    m_socket = std::make_unique<TcpSocket>(IPType::IPV4);
    m_ring_buffer = std::make_unique<RingBuffer>(m_ring_buffer_size);

    auto nonblock_result = m_socket->option().handleNonBlock();
    if (!nonblock_result) {
        throw std::runtime_error("Failed to set non-blocking: " + nonblock_result.error().message());
    }

    Host server_host(IPType::IPV4, m_url.host, m_url.port);
    return m_socket->connect(server_host);
}

WsClientUpgradeAwaitable& WsClient::upgrade() {
    if (!m_socket) {
        throw std::runtime_error("WsClient not connected. Call connect() first.");
    }

    // 如果 awaitable 不存在或已完成（Invalid 状态），创建新的
    if (!m_upgrade_awaitable || m_upgrade_awaitable->isInvalid()) {
        m_upgrade_awaitable = std::make_unique<WsClientUpgradeAwaitable>(this);
    }

    return *m_upgrade_awaitable;
}



CloseAwaitable WsClient::close() {
    if (!m_ws_conn) {
        throw std::runtime_error("WsClient not connected");
    }
    return m_ws_conn->close();
}

// ==================== WsClientUpgradeAwaitable 实现 ====================

WsClientUpgradeAwaitable::WsClientUpgradeAwaitable(WsClient* client)
    : m_client(client)
    , m_state(State::Invalid)
    , m_send_offset(0)
{
}

bool WsClientUpgradeAwaitable::await_suspend(std::coroutine_handle<> handle)
{
    if (m_state == State::Invalid) {
        // Invalid 状态，开始发送升级请求
        m_state = State::Sending;

        // 生成 WebSocket Key
        m_ws_key = generateWebSocketKey();
        HTTP_LOG_DEBUG("Generated WebSocket-Key: {}", m_ws_key);

        // 构建升级请求
        m_upgrade_request = Http1_1RequestBuilder::get(m_client->m_url.path)
            .host(m_client->m_url.host + ":" + std::to_string(m_client->m_url.port))
            .header("Connection", "Upgrade")
            .header("Upgrade", "websocket")
            .header("Sec-WebSocket-Version", "13")
            .header("Sec-WebSocket-Key", m_ws_key)
            .build();

        // 序列化请求
        m_send_buffer = m_upgrade_request.toString();
        m_send_offset = 0;

        HTTP_LOG_INFO("Sending WebSocket upgrade request...");

        // 创建 send awaitable
        size_t remaining = m_send_buffer.size() - m_send_offset;
        const char* send_ptr = m_send_buffer.data() + m_send_offset;
        m_send_awaitable.emplace(m_client->m_socket->send(send_ptr, remaining));
        return m_send_awaitable->await_suspend(handle);

    } else if (m_state == State::Sending) {
        // 继续发送请求（重新创建 awaitable）
        size_t remaining = m_send_buffer.size() - m_send_offset;
        const char* send_ptr = m_send_buffer.data() + m_send_offset;
        m_send_awaitable.emplace(m_client->m_socket->send(send_ptr, remaining));
        return m_send_awaitable->await_suspend(handle);

    } else {
        // Receiving 状态，接收响应（重新创建 awaitable）
        m_recv_awaitable.emplace(m_client->m_socket->readv(m_client->m_ring_buffer->getWriteIovecs()));
        return m_recv_awaitable->await_suspend(handle);
    }
}

std::expected<bool, WsError> WsClientUpgradeAwaitable::await_resume()
{
    if (m_state == State::Sending) {
        // 检查发送结果
        auto send_result = m_send_awaitable->await_resume();

        if (!send_result) {
            // 发送错误
            HTTP_LOG_ERROR("Failed to send upgrade request: {}", send_result.error().message());
            reset();
            return std::unexpected(WsError(kWsConnectionError,
                "Failed to send upgrade request: " + send_result.error().message()));
        }

        m_send_offset += send_result.value();

        if (m_send_offset < m_send_buffer.size()) {
            // 发送未完成，返回 false 继续发送
            HTTP_LOG_DEBUG("Sent {} / {} bytes", m_send_offset, m_send_buffer.size());
            return false;
        }

        // 发送完成，切换到 Receiving 状态
        HTTP_LOG_INFO("Upgrade request sent, waiting for response...");
        m_state = State::Receiving;
        m_send_awaitable.reset();  // 清理发送 awaitable
        return false;

    } else if (m_state == State::Receiving) {
        // 检查接收结果
        auto recv_result = m_recv_awaitable->await_resume();

        if (!recv_result) {
            // 接收错误
            HTTP_LOG_ERROR("Failed to receive upgrade response: {}", recv_result.error().message());
            reset();
            return std::unexpected(WsError(kWsConnectionError,
                "Failed to receive upgrade response: " + recv_result.error().message()));
        }

        // 更新 ring buffer
        m_client->m_ring_buffer->produce(recv_result.value());
        HTTP_LOG_DEBUG("Received {} bytes", recv_result.value());

        // 尝试解析 HTTP 响应
        auto parse_result = m_upgrade_response.fromIOVec(
            m_client->m_ring_buffer->getReadIovecs());

        if (parse_result.first != HttpErrorCode::kNoError) {
            // 解析失败
            HTTP_LOG_ERROR("Failed to parse upgrade response: error code {}",
                          static_cast<int>(parse_result.first));
            reset();
            return std::unexpected(WsError(kWsProtocolError,
                "Failed to parse upgrade response"));
        }

        if (!m_upgrade_response.isComplete()) {
            // 响应不完整，返回 false 继续接收
            HTTP_LOG_DEBUG("Response incomplete, continue receiving");
            return false;
        }

        // 响应完整，验证升级
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
            HTTP_LOG_ERROR("Expected: {}", expected_accept);
            HTTP_LOG_ERROR("Received: {}", accept_key);
            reset();
            return std::unexpected(WsError(kWsUpgradeFailed,
                "Invalid Sec-WebSocket-Accept value"));
        }

        HTTP_LOG_INFO("WebSocket upgrade successful!");
        HTTP_LOG_DEBUG("Sec-WebSocket-Accept verified");

        // 消费已解析的数据
        size_t consumed = parse_result.second;
        m_client->m_ring_buffer->consume(consumed);

        // 检查是否有剩余数据（可能是服务端提前发送的 WebSocket 帧）
        if (m_client->m_ring_buffer->readable() > 0) {
            HTTP_LOG_DEBUG("Ring buffer has {} bytes remaining after upgrade (may contain WebSocket frames)",
                         m_client->m_ring_buffer->readable());
        }

        // 创建 WebSocket 连接（直接使用原始 socket 和 ring buffer，保留剩余数据）
        m_client->m_ws_conn = std::make_unique<WsConn>(
            std::move(*m_client->m_socket),
            std::move(*m_client->m_ring_buffer),
            m_client->m_reader_setting,
            m_client->m_writer_setting,
            false  // is_server = false (客户端)
        );

        HTTP_LOG_INFO("WsConn created successfully");

        // 清理资源（socket 和 ring_buffer 已经移动到 WsConn，这里只是清理 unique_ptr）
        m_client->m_socket.reset();
        m_client->m_ring_buffer.reset();

        // 升级完成，释放 awaitable（节省内存）
        m_client->m_upgrade_awaitable.reset();

        return true;  // 升级成功

    } else {
        // Invalid 状态，不应该被调用
        HTTP_LOG_ERROR("await_resume called in Invalid state");
        reset();
        return std::unexpected(WsError(kWsProtocolError, "Invalid state"));
    }
}

} // namespace galay::websocket
