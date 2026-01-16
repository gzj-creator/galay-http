#include "HttpWriter.h"
#include "HttpLog.h"

namespace galay::http
{

std::expected<size_t, HttpError> ResponseAwaitable::await_resume()
{
    // 获取WritevAwaitable的结果
    auto writev_result = m_writev_awaitable.await_resume();
    if (!writev_result) {
        // IO错误
        HTTP_LOG_DEBUG("writev failed: {}", writev_result.error().message());
        return std::unexpected(HttpError(kSendError, writev_result.error().message()));
    }

    size_t bytes_written = writev_result.value();
    HTTP_LOG_DEBUG("sent {} bytes", bytes_written);

    return bytes_written;
}

} // namespace galay::http
