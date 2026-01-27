#ifndef GALAY_WS_CLIENT_H
#define GALAY_WS_CLIENT_H

#include "WsConn.h"
#include "WsReader.h"
#include "WsWriter.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/common/Buffer.h"
#include "galay-kernel/kernel/Awaitable.h"
#include <string>
#include <optional>
#include <expected>
#include <memory>

namespace galay::websocket
{

using namespace galay::async;
using namespace galay::kernel;
using namespace galay::http;

/**
 * @brief WebSocket URL 解析结果
 */
struct WsUrl {
    std::string scheme;   // "ws" 或 "wss"
    std::string host;     // 主机名或 IP
    int port;             // 端口号
    std::string path;     // 路径（包含查询参数）
    bool is_secure;       // 是否是 wss://

    /**
     * @brief 解析 WebSocket URL
     * @param url WebSocket URL 字符串（如 "ws://127.0.0.1:8080/ws"）
     * @return std::optional<WsUrl> 解析成功返回 WsUrl，失败返回 nullopt
     */
    static std::optional<WsUrl> parse(const std::string& url);
};

// 前向声明
class WsClient;

/**
 * @brief WebSocket 客户端升级等待体
 * @details 参考 HttpClientAwaitable 的设计，使用状态机管理升级流程
 */
class WsClientUpgradeAwaitable
{
public:
    WsClientUpgradeAwaitable(WsClient* client);

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle);

    /**
     * @brief 恢复协程时调用
     * @return std::expected<bool, WsError>
     *         - true: 升级成功
     *         - false: 需要继续等待（返回 nullopt 的语义）
     *         - WsError: 升级失败
     */
    std::expected<bool, WsError> await_resume();

    /**
     * @brief 检查状态是否为 Invalid
     */
    bool isInvalid() const {
        return m_state == State::Invalid;
    }

    /**
     * @brief 重置状态并清理资源
     */
    void reset() {
        m_state = State::Invalid;
        m_send_awaitable.reset();
        m_recv_awaitable.reset();
        m_send_buffer.clear();
        m_send_offset = 0;
    }

private:
    enum class State {
        Invalid,           // 无效状态
        Sending,           // 正在发送升级请求
        Receiving          // 正在接收升级响应
    };

    WsClient* m_client;
    State m_state;

    // 持有底层的 TcpSocket awaitable
    std::optional<SendAwaitable> m_send_awaitable;
    std::optional<ReadvAwaitable> m_recv_awaitable;

    // 发送缓冲区
    std::string m_send_buffer;
    size_t m_send_offset;

    // 升级过程中的数据（从 WsClient 移动过来）
    std::string m_ws_key;
    HttpRequest m_upgrade_request;
    HttpResponse m_upgrade_response;
};

/**
 * @brief WebSocket 客户端
 * @details 提供简化的 WebSocket 客户端接口，负责连接管理和 URL 解析
 *          实际的收发操作通过内部的 WsConn 进行
 *
 * @example
 * @code
 * Coroutine example() {
 *     WsClient client;
 *
 *     // 1. 连接到服务器（TCP 连接）
 *     auto connect_result = co_await client.connect("ws://127.0.0.1:8080/ws");
 *     if (!connect_result) {
 *         HTTP_LOG_ERROR("Failed to connect: {}", connect_result.error().message());
 *         co_return;
 *     }
 *
 *     // 2. WebSocket 握手升级
 *     while (true) {
 *         auto upgrade_result = co_await client.upgrade();
 *         if (!upgrade_result.has_value()) {
 *             HTTP_LOG_ERROR("Upgrade failed: {}", upgrade_result.error().message());
 *             co_return;
 *         }
 *         if (upgrade_result.value()) {
 *             // 升级成功
 *             break;
 *         }
 *         // 继续等待
 *     }
 *
 *     // 3. 发送和接收消息
 *     co_await client.sendText("Hello Server!");
 *
 *     std::string message;
 *     WsOpcode opcode;
 *     co_await client.getMessage(message, opcode);
 *
 *     // 4. 关闭连接
 *     co_await client.close();
 * }
 * @endcode
 */
class WsClient
{
public:
    /**
     * @brief 构造函数
     * @param reader_setting WebSocket 读取器配置
     * @param writer_setting WebSocket 写入器配置
     * @param ring_buffer_size RingBuffer 大小（默认 8192）
     */
    WsClient(const WsReaderSetting& reader_setting = WsReaderSetting(),
             const WsWriterSetting& writer_setting = WsWriterSetting(),
             size_t ring_buffer_size = 8192);

    /**
     * @brief 析构函数
     */
    ~WsClient() = default;

    // 禁用拷贝
    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;

    // 禁用移动
    WsClient(WsClient&&) = delete;
    WsClient& operator=(WsClient&&) = delete;

    /**
     * @brief 连接到 WebSocket 服务器（TCP 连接）
     * @param url WebSocket URL（如 "ws://127.0.0.1:8080/ws"）
     * @return ConnectAwaitable TCP 连接等待体
     * @note 连接成功后，需要调用 upgrade() 进行 WebSocket 握手
     */
    ConnectAwaitable connect(const std::string& url);

    /**
     * @brief WebSocket 握手升级
     * @return WsClientUpgradeAwaitable 升级等待体
     * @note 需要在循环中调用，直到返回 true 表示升级成功
     */
    WsClientUpgradeAwaitable& upgrade();

    /**
     * @brief 检查是否已连接
     * @return true 已连接，false 未连接
     */
    bool isConnected() const {
        return m_ws_conn != nullptr;
    }

    /**
     * @brief 关闭连接
     * @return CloseAwaitable 关闭等待体
     */
    CloseAwaitable close();

    /**
     * @brief 设置读取器配置
     * @note 必须在 connect() 之前调用
     */
    void setReaderSetting(const WsReaderSetting& setting) {
        m_reader_setting = setting;
    }

    /**
     * @brief 设置写入器配置
     * @note 必须在 connect() 之前调用
     */
    void setWriterSetting(const WsWriterSetting& setting) {
        m_writer_setting = setting;
    }

    /**
     * @brief 获取 WebSocket 读取器
     * @return WsReader& 读取器引用
     * @note 必须在 upgrade() 成功后才能使用
     * @note 使用示例：
     * @code
     * std::string message;
     * WsOpcode opcode;
     * co_await client.getWsReader().getMessage(message, opcode);
     * @endcode
     */
    WsReader& getWsReader() {
        return m_ws_conn->getReader();
    }

    /**
     * @brief 获取 WebSocket 写入器
     * @return WsWriter& 写入器引用
     * @note 必须在 upgrade() 成功后才能使用
     * @note 使用示例：
     * @code
     * co_await client.getWsWriter().sendText("Hello");
     * @endcode
     */
    WsWriter& getWsWriter() {
        return m_ws_conn->getWriter();
    }

    /**
     * @brief 获取底层 WsConn（高级用户使用）
     * @return WsConn* 连接指针，未连接时返回 nullptr
     */
    WsConn* getConn() {
        return m_ws_conn.get();
    }

    // 允许 WsClientUpgradeAwaitable 访问私有成员
    friend class WsClientUpgradeAwaitable;

private:
    WsReaderSetting m_reader_setting;
    WsWriterSetting m_writer_setting;
    size_t m_ring_buffer_size;

    // 连接相关（升级前使用，升级后移动到 WsConn）
    std::unique_ptr<TcpSocket> m_socket;
    std::unique_ptr<RingBuffer> m_ring_buffer;

    // WebSocket 连接（升级后使用）
    std::unique_ptr<WsConn> m_ws_conn;

    // URL 信息
    WsUrl m_url;

    // 升级等待体（只在升级过程中使用，完成后释放）
    std::unique_ptr<WsClientUpgradeAwaitable> m_upgrade_awaitable;
};

} // namespace galay::websocket

#endif // GALAY_WS_CLIENT_H
