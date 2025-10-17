#ifndef GALAY_WS_PARAMS_H
#define GALAY_WS_PARAMS_H

#include "galay-http/protoc/websocket/WsBase.h"

namespace galay::http
{
    struct WsSettings {
        std::chrono::milliseconds recv_timeout = DEFAULT_WS_RECV_TIMEOUT;
        std::chrono::milliseconds send_timeout = DEFAULT_WS_SEND_TIMEOUT;
        
        size_t max_frame_size = DEFAULT_WS_MAX_FRAME_SIZE;      // 单个帧的最大大小
        size_t max_message_size = DEFAULT_WS_MAX_FRAME_SIZE;    // 完整消息的最大大小
        size_t recv_buffer_size = 4096;                         // 接收缓冲区大小
        
        std::chrono::milliseconds ping_interval = DEFAULT_WS_PING_INTERVAL;  // Ping 间隔
        std::chrono::milliseconds pong_timeout = DEFAULT_WS_PONG_TIMEOUT;    // Pong 超时
        
        bool auto_ping = true;          // 是否自动发送 Ping
        bool auto_pong = true;          // 是否自动回复 Pong
        bool validate_utf8 = true;      // 是否验证 UTF-8（文本帧）
    };
}

#endif

