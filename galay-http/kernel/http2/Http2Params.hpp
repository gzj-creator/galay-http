#ifndef GALAY_HTTP2_PARAMS_H
#define GALAY_HTTP2_PARAMS_H

#include "galay-http/protoc/http2/Http2Base.h"

namespace galay::http
{
    /**
     * @brief HTTP/2 连接配置参数
     * 
     * 类似于 WsSettings，提供 HTTP/2 连接的各种配置选项
     */
    struct Http2Settings {
        // ==================== 超时设置 ====================
        
        /// 接收数据超时时间（默认 30 秒）
        std::chrono::milliseconds recv_timeout = DEFAULT_HTTP2_RECV_TIMEOUT;
        
        /// 发送数据超时时间（默认 30 秒）
        std::chrono::milliseconds send_timeout = DEFAULT_HTTP2_SEND_TIMEOUT;
        
        /// SETTINGS 帧确认超时（默认 5 秒）
        std::chrono::milliseconds settings_timeout = std::chrono::milliseconds(5000);
        
        // ==================== 帧大小限制 ====================
        
        /// 接收帧的最大大小（默认 16KB，最大 16MB）
        /// 对应 SETTINGS_MAX_FRAME_SIZE
        uint32_t max_frame_size = DEFAULT_HTTP2_MAX_FRAME_SIZE;
        
        /// 头部列表的最大大小（默认 8KB）
        /// 对应 SETTINGS_MAX_HEADER_LIST_SIZE
        uint32_t max_header_list_size = DEFAULT_HTTP2_MAX_HEADER_LIST_SIZE;
        
        // ==================== 流控制设置 ====================
        
        /// 初始窗口大小（默认 65535，即 64KB - 1）
        /// 对应 SETTINGS_INITIAL_WINDOW_SIZE
        uint32_t initial_window_size = DEFAULT_HTTP2_INITIAL_WINDOW_SIZE;
        
        /// 连接级别的窗口大小（默认 65535）
        uint32_t connection_window_size = DEFAULT_HTTP2_INITIAL_WINDOW_SIZE;
        
        // ==================== 并发控制 ====================
        
        /// 最大并发流数量（默认 100）
        /// 对应 SETTINGS_MAX_CONCURRENT_STREAMS
        uint32_t max_concurrent_streams = DEFAULT_HTTP2_MAX_CONCURRENT_STREAMS;
        
        // ==================== HPACK 压缩设置 ====================
        
        /// HPACK 动态表大小（默认 4096）
        /// 对应 SETTINGS_HEADER_TABLE_SIZE
        uint32_t header_table_size = 4096;
        
        /// 是否启用 HPACK 压缩（默认 true）
        bool enable_hpack = true;
        
        // ==================== 服务器推送设置 ====================
        
        /// 是否启用服务器推送（默认 false）
        /// 对应 SETTINGS_ENABLE_PUSH
        bool enable_push = false;
        
        // ==================== 缓冲区设置 ====================
        
        /// 接收缓冲区大小（默认 64KB）
        size_t recv_buffer_size = 65536;
        
        /// 发送缓冲区大小（默认 64KB）
        size_t send_buffer_size = 65536;
        
        // ==================== 其他设置 ====================
        
        /// 是否启用流优先级（默认 false）
        bool enable_priority = false;
        
        /// 是否自动发送 PING 帧（心跳检测，默认 true）
        bool auto_ping = true;
        
        /// PING 间隔时间（默认 30 秒）
        std::chrono::milliseconds ping_interval = std::chrono::milliseconds(30000);
        
        /// PING 超时时间（默认 10 秒）
        std::chrono::milliseconds ping_timeout = std::chrono::milliseconds(10000);
        
        /// 是否自动发送 WINDOW_UPDATE（默认 true）
        bool auto_window_update = true;
        
        /// 窗口更新阈值（当窗口小于此值时自动发送 WINDOW_UPDATE）
        /// 默认为初始窗口大小的一半
        uint32_t window_update_threshold = DEFAULT_HTTP2_INITIAL_WINDOW_SIZE / 2;
    };
}

#endif // GALAY_HTTP2_PARAMS_H

