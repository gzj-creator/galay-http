#ifndef GALAY_HTTP2_FRAME_DISPATCHER_H
#define GALAY_HTTP2_FRAME_DISPATCHER_H

#include "galay-http/protoc/http2/http2_frame.h"
#include <vector>

namespace galay::http2
{

enum class H2DispatchActionType
{
    SendGoaway,
    SendRstStream
};

struct H2DispatchAction
{
    H2DispatchActionType type;
    uint32_t stream_id = 0;
    Http2ErrorCode error_code = Http2ErrorCode::NoError;
};

struct H2DispatchResult
{
    bool ok = true;
    std::vector<H2DispatchAction> actions;
};

struct H2DispatcherConnectionState
{
    bool expecting_continuation = false;
    uint32_t continuation_stream_id = 0;
    bool goaway_received = false;
};

/**
 * @brief 连接级帧分发器（重写阶段最小状态机）
 */
class Http2FrameDispatcher
{
public:
    static H2DispatchResult dispatch(const Http2Frame& frame,
                                     H2DispatcherConnectionState& state);
};

} // namespace galay::http2

#endif // GALAY_HTTP2_FRAME_DISPATCHER_H
