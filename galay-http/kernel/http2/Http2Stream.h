#ifndef GALAY_HTTP2_STREAM_H
#define GALAY_HTTP2_STREAM_H

#include "galay-http/protoc/http2/Http2Base.h"
#include "galay-http/protoc/http2/Http2Error.h"
#include "Http2Params.hpp"
#include <cstdint>
#include <string>
#include <memory>
#include <expected>
#include <unordered_map>
#include <queue>
#include <vector>

namespace galay::http
{
    class Http2Connection;
    class Http2Header;
}

namespace galay::http
{
    /**
     * @brief HTTP/2 流
     * 
     * 表示一个独立的双向数据流
     * 类似于 WebSocket 的连接，但 HTTP/2 可以在同一个 TCP 连接上有多个流
     */
    class Http2Stream : public std::enable_shared_from_this<Http2Stream>
    {
    public:
        using ptr = std::shared_ptr<Http2Stream>;
        
        Http2Stream(uint32_t stream_id, uint32_t initial_window_size, Http2Connection& connection);
        ~Http2Stream() = default;
        
        // ==================== 基本信息 ====================
        
        uint32_t streamId() const { return m_stream_id; }
        Http2StreamState state() const { return m_state; }
        
        // ==================== 状态管理 ====================
        
        void setState(Http2StreamState state) { m_state = state; }
        
        // 是否可以发送数据
        bool canSendData() const;
        
        // 是否可以接收数据
        bool canReceiveData() const;
        
        // 是否已关闭
        bool isClosed() const { return m_state == Http2StreamState::CLOSED; }
        
        // ==================== 流控管理 ====================
        
        // 获取发送窗口大小
        int32_t sendWindowSize() const { return m_send_window_size; }
        
        // 获取接收窗口大小
        int32_t recvWindowSize() const { return m_recv_window_size; }
        
        // 更新发送窗口（收到 WINDOW_UPDATE）
        std::expected<void, Http2Error> updateSendWindow(uint32_t increment);
        
        // 更新接收窗口（发送 WINDOW_UPDATE）
        std::expected<void, Http2Error> updateRecvWindow(uint32_t increment);
        
        // 消耗发送窗口（发送数据时）
        std::expected<void, Http2Error> consumeSendWindow(uint32_t size);
        
        // 消耗接收窗口（接收数据时）
        std::expected<void, Http2Error> consumeRecvWindow(uint32_t size);
        
        // ==================== 数据缓冲 ====================
        
        // 添加接收到的数据
        void appendReceivedData(const std::string& data);
        
        // 获取已接收的数据
        std::string getReceivedData();
        
        // 清空接收缓冲区
        void clearReceivedData();
        
        // 接收缓冲区大小
        size_t receivedDataSize() const { return m_received_data.size(); }
        
        // ==================== 头部处理 ====================
        
        // 设置请求/响应头部
        void setHeaders(const std::string& header_block);
        
        // 获取头部
        const std::string& headers() const { return m_headers; }
        
        // 是否已收到头部
        bool hasHeaders() const { return !m_headers.empty(); }
        
        // ==================== 优先级 ====================
        
        void setPriority(uint32_t dependency, uint8_t weight, bool exclusive = false);
        uint32_t dependency() const { return m_dependency; }
        uint8_t weight() const { return m_weight; }
        bool exclusive() const { return m_exclusive; }
        
        // ==================== 错误处理 ====================
        
        void setError(Http2ErrorCode error_code);
        Http2ErrorCode errorCode() const { return m_error_code; }
        bool hasError() const { return m_error_code != Http2ErrorCode::NO_ERROR; }
        
        // ==================== 连接访问 ====================
        
        Http2Connection& connection();
        
    private:
        friend class Http2StreamManager;
        friend class Http2StreamHelper;
        
        uint32_t m_stream_id;               // 流 ID
        Http2StreamState m_state;           // 流状态
        
        // 流控
        int32_t m_send_window_size;         // 发送窗口大小
        int32_t m_recv_window_size;         // 接收窗口大小
        
        // 数据缓冲
        std::string m_received_data;        // 接收到的数据
        std::string m_headers;              // 头部（HPACK 压缩后的）
        
        // 优先级
        uint32_t m_dependency;              // 依赖的流 ID
        uint8_t m_weight;                   // 权重
        bool m_exclusive;                   // 是否独占
        
        // 错误
        Http2ErrorCode m_error_code;        // 错误码
        
        Http2Connection* m_connection;      // 关联的 HTTP/2 连接
    };
    
    /**
     * @brief HTTP/2 流管理器
     * 
     * 管理连接上的所有流
     */
    class Http2StreamManager
    {
    public:
        Http2StreamManager(Http2Connection& connection, const Http2Settings& settings);
        ~Http2StreamManager() = default;
        
        // ==================== 流创建和销毁 ====================
        
        // 创建新流
        std::expected<Http2Stream::ptr, Http2Error> createStream(uint32_t stream_id);
        
        // 获取流
        Http2Stream::ptr getStream(uint32_t stream_id);
        
        // 删除流
        void removeStream(uint32_t stream_id);
        
        // 关闭流
        void closeStream(uint32_t stream_id);
        
        // ==================== 流 ID 管理 ====================
        
        // 获取下一个可用的流 ID（客户端使用奇数，服务器使用偶数）
        uint32_t getNextStreamId();
        
        // 设置是否为服务器端
        void setServerMode(bool is_server) { m_is_server = is_server; }
        
        // ==================== 流控管理 ====================
        
        // 更新连接级别的发送窗口
        std::expected<void, Http2Error> updateConnectionSendWindow(uint32_t increment);
        
        // 更新连接级别的接收窗口
        std::expected<void, Http2Error> updateConnectionRecvWindow(uint32_t increment);
        
        // 消耗连接级别的发送窗口
        std::expected<void, Http2Error> consumeConnectionSendWindow(uint32_t size);
        
        // 消耗连接级别的接收窗口
        std::expected<void, Http2Error> consumeConnectionRecvWindow(uint32_t size);
        
        int32_t connectionSendWindow() const { return m_connection_send_window; }
        int32_t connectionRecvWindow() const { return m_connection_recv_window; }
        
        // ==================== 流统计 ====================
        
        size_t activeStreamCount() const { return m_streams.size(); }
        uint32_t maxConcurrentStreams() const { return m_max_concurrent_streams; }
        
        // ==================== 优先级调度 ====================
        
        // 获取下一个待调度的流（按优先级）
        Http2Stream::ptr getNextStreamToSchedule();
        
        // 更新流的优先级（会重新排序优先级队列）
        void updateStreamPriority(uint32_t stream_id);
        
        // ==================== 设置更新 ====================
        
        void updateSettings(const Http2Settings& settings);
        
    private:
        // 优先级比较器
        struct StreamPriorityComparator {
            bool operator()(const Http2Stream::ptr& a, const Http2Stream::ptr& b) const;
        };
        
        // 计算流的优先级值（用于排序）
        // 返回值越小，优先级越高
        uint64_t calculatePriorityValue(const Http2Stream::ptr& stream) const;
        
        // 重建优先级队列（当流的优先级改变时调用）
        void rebuildPriorityQueue();
        
        bool m_is_server;                   // 是否为服务器端
        uint32_t m_next_stream_id;          // 下一个流 ID
        uint32_t m_max_concurrent_streams;  // 最大并发流数
        uint32_t m_initial_window_size;     // 初始窗口大小
        
        // 连接级别的流控窗口
        int32_t m_connection_send_window;   // 连接发送窗口
        int32_t m_connection_recv_window;   // 连接接收窗口
        
        // 流映射
        std::unordered_map<uint32_t, Http2Stream::ptr> m_streams;
        
        // 优先级队列
        std::priority_queue<Http2Stream::ptr, 
                           std::vector<Http2Stream::ptr>, 
                           StreamPriorityComparator> m_priority_queue;
        bool m_priority_queue_dirty;        // 标记优先级队列是否需要重建
        
        Http2Connection* m_connection;      // 关联的连接
    };
}

#endif // GALAY_HTTP2_STREAM_H

