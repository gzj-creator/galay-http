#ifndef GALAY_HTTP2_OUTBOUND_SCHEDULER_H
#define GALAY_HTTP2_OUTBOUND_SCHEDULER_H

#include "galay-http/protoc/http2/http2_frame.h"
#include <cstdint>
#include <string>
#include <vector>

namespace galay::http2
{

struct H2OutboundBudget
{
    int32_t conn_window = 0;
    uint32_t max_frame_size = kDefaultMaxFrameSize;
};

struct H2StreamSendState
{
    uint32_t stream_id = 0;
    int32_t stream_window = 0;
    std::string pending_data;
    bool end_stream = false;
    uint8_t weight = 16;
};

struct H2OutboundSelection
{
    std::vector<Http2Frame::uptr> frames;
    size_t total_data_bytes = 0;
};

/**
 * @brief 连接级出站调度器（重写阶段最小实现）
 */
class Http2OutboundScheduler
{
public:
    static H2OutboundSelection pickSendableFrames(H2OutboundBudget budget,
                                                  std::vector<H2StreamSendState>& streams);
};

} // namespace galay::http2

#endif // GALAY_HTTP2_OUTBOUND_SCHEDULER_H
