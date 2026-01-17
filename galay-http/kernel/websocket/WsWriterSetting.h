#ifndef GALAY_WS_WRITER_SETTING_H
#define GALAY_WS_WRITER_SETTING_H

#include <cstddef>

namespace galay::websocket
{

/**
 * @brief WebSocket写入器配置
 */
struct WsWriterSetting
{
    size_t max_frame_size;       // 单个帧的最大大小（默认 10MB）
    bool auto_fragment;          // 是否自动分片大消息（默认 true）
    bool use_mask;               // 是否使用掩码（客户端必须为true，服务器必须为false）

    WsWriterSetting(bool is_client = false)
        : max_frame_size(10 * 1024 * 1024)  // 10MB
        , auto_fragment(true)
        , use_mask(is_client)
    {
    }
};

} // namespace galay::websocket

#endif // GALAY_WS_WRITER_SETTING_H
