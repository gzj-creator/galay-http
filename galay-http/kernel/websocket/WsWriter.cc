#include "WsWriter.h"

namespace galay::websocket
{

std::expected<size_t, WsError> SendFrameAwaitable::await_resume()
{
    auto result = m_send_awaitable.await_resume();

    if (!result.has_value()) {
        // 发送失败，转换为 WebSocket 错误
        return std::unexpected(WsError(kWsConnectionClosed, "Socket send failed"));
    }

    size_t bytes_sent = result.value();
    m_writer.updateRemaining(bytes_sent);

    return bytes_sent;
}

} // namespace galay::websocket
