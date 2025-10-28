#ifndef GALAY_HTTP2_CALLBACKS_H
#define GALAY_HTTP2_CALLBACKS_H

#include "galay-http/protoc/http2/Http2Frame.h"
#include "galay-http/protoc/http2/Http2Error.h"
#include "Http2Connection.h"
#include "Http2Stream.h"
#include <functional>
#include <map>
#include <string>

namespace galay::http
{
    // 前向声明
    class Http2Connection;
    class Http2Stream;
    
    /**
     * @brief HTTP/2 回调函数类型定义
     * 
     * 这些回调函数在收到对应类型的帧时被调用
     * 所有回调函数都返回 bool：
     *   - true: 继续处理
     *   - false: 停止处理并关闭连接
     */
    
    /**
     * @brief HEADERS 帧回调
     * @param connection HTTP/2 连接
     * @param stream_id 流 ID
     * @param headers 解析后的头部字段
     * @param end_stream 是否是流的最后一帧
     * 注：如需关闭连接，请在回调中发送 GOAWAY 帧
     */
    using OnHeadersCallback = std::function<Coroutine<nil>(
        Http2Connection& connection,
        uint32_t stream_id,
        const std::map<std::string, std::string>& headers,
        bool end_stream
    )>;
    
    /**
     * @brief DATA 帧回调
     * @param connection HTTP/2 连接
     * @param stream_id 流 ID
     * @param data 数据内容
     * @param end_stream 是否是流的最后一帧
     * 注：如需关闭连接，请在回调中发送 GOAWAY 帧
     */
    using OnDataCallback = std::function<Coroutine<nil>(
        Http2Connection& connection,
        uint32_t stream_id,
        const std::string& data,
        bool end_stream
    )>;
    
    /**
     * @brief SETTINGS 帧回调
     * @param connection HTTP/2 连接
     * @param settings 设置参数
     * @param is_ack 是否是 ACK 帧
     * 注：SETTINGS ACK 会自动发送，无需在回调中处理
     */
    using OnSettingsCallback = std::function<Coroutine<nil>(
        Http2Connection& connection,
        const std::map<Http2SettingsId, uint32_t>& settings,
        bool is_ack
    )>;
    
    /**
     * @brief PING 帧回调
     * @param connection HTTP/2 连接
     * @param data Ping 数据（64位整数）
     * @param is_ack 是否是 ACK 帧
     * 注：PING ACK 会自动发送，无需在回调中处理
     */
    using OnPingCallback = std::function<Coroutine<nil>(
        Http2Connection& connection,
        uint64_t data,
        bool is_ack
    )>;
    
    /**
     * @brief GOAWAY 帧回调
     * @param connection HTTP/2 连接
     * @param last_stream_id 最后处理的流 ID
     * @param error_code 错误码
     * @param debug_data 调试信息
     * 注：收到 GOAWAY 后连接会自动关闭
     */
    using OnGoawayCallback = std::function<Coroutine<nil>(
        Http2Connection& connection,
        uint32_t last_stream_id,
        Http2ErrorCode error_code,
        const std::string& debug_data
    )>;
    
    /**
     * @brief WINDOW_UPDATE 帧回调
     * @param connection HTTP/2 连接
     * @param stream_id 流 ID（0 表示连接级别）
     * @param window_size_increment 窗口大小增量
     */
    using OnWindowUpdateCallback = std::function<Coroutine<nil>(
        Http2Connection& connection,
        uint32_t stream_id,
        uint32_t window_size_increment
    )>;
    
    /**
     * @brief RST_STREAM 帧回调
     * @param connection HTTP/2 连接
     * @param stream_id 流 ID
     * @param error_code 错误码
     */
    using OnRstStreamCallback = std::function<Coroutine<nil>(
        Http2Connection& connection,
        uint32_t stream_id,
        Http2ErrorCode error_code
    )>;
    
    /**
     * @brief PRIORITY 帧回调
     * @param connection HTTP/2 连接
     * @param stream_id 流 ID
     * @param depends_on 依赖的流 ID
     * @param weight 权重
     * @param exclusive 是否独占
     */
    using OnPriorityCallback = std::function<Coroutine<nil>(
        Http2Connection& connection,
        uint32_t stream_id,
        uint32_t depends_on,
        uint8_t weight,
        bool exclusive
    )>;
    
    /**
     * @brief 错误回调
     * @param connection HTTP/2 连接
     * @param error 错误信息
     */
    using OnErrorCallback = std::function<Coroutine<nil>(
        Http2Connection& connection,
        const Http2Error& error
    )>;
    
    /**
     * @brief HTTP/2 回调集合
     * 
     * 用于配置服务器的帧处理回调
     * 所有回调都是可选的，如果不设置则使用默认行为
     */
    struct Http2Callbacks
    {
        // 必需的回调
        OnHeadersCallback on_headers = nullptr;  // HEADERS 帧
        OnDataCallback on_data = nullptr;        // DATA 帧
        
        // 可选的回调（有默认实现）
        OnSettingsCallback on_settings = nullptr;          // SETTINGS 帧
        OnPingCallback on_ping = nullptr;                  // PING 帧
        OnGoawayCallback on_goaway = nullptr;              // GOAWAY 帧
        OnWindowUpdateCallback on_window_update = nullptr; // WINDOW_UPDATE 帧
        OnRstStreamCallback on_rst_stream = nullptr;       // RST_STREAM 帧
        OnPriorityCallback on_priority = nullptr;          // PRIORITY 帧
        
        // 错误处理
        OnErrorCallback on_error = nullptr;
        
        /**
         * @brief 验证回调是否有效
         * @return true 如果至少设置了 on_headers 和 on_data
         */
        bool isValid() const
        {
            return on_headers != nullptr && on_data != nullptr;
        }
        
        /**
         * @brief 创建默认回调
         * @return 一个具有默认实现的回调集合
         */
        static Http2Callbacks createDefault();
    };
}

#endif // GALAY_HTTP2_CALLBACKS_H

