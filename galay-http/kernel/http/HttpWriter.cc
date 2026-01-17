#include "HttpWriter.h"
#include "HttpLog.h"

namespace galay::http
{

std::expected<size_t, HttpError> ResponseAwaitable::await_resume()
{
    // 获取SendAwaitable的结果
    auto send_result = m_send_awaitable.await_resume();
    if (!send_result) {
        // IO错误
        HTTP_LOG_DEBUG("send failed: {}", send_result.error().message());
        return std::unexpected(HttpError(kSendError, send_result.error().message()));
    }

    size_t bytes_written = send_result.value();
    HTTP_LOG_DEBUG("sent {} bytes", bytes_written);

    return bytes_written;
}

} // namespace galay::http
