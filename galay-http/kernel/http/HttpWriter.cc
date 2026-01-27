#include "HttpWriter.h"
#include "HttpLog.h"
#include "galay-http/protoc/http/HttpChunk.h"

namespace galay::http
{

std::expected<bool, HttpError> SendResponseAwaitable::await_resume()
{
    // 获取SendAwaitable的结果
    auto send_result = m_send_awaitable.await_resume();
    if (!send_result) {
        // IO错误
        HTTP_LOG_DEBUG("send failed: {}", send_result.error().message());
        return std::unexpected(HttpError(kSendError, send_result.error().message()));
    }

    size_t bytes_written = send_result.value();

    // 更新Writer的剩余发送字节数
    m_writer.updateRemaining(bytes_written);

    // 检查是否发送完成
    if (m_writer.getRemainingBytes() == 0) {
        return true;
    }

    // 还有数据需要发送
    return false;
}

SendResponseAwaitable HttpWriter::sendChunk(const std::string& data, bool is_last)
{
    // 只在第一次调用时（剩余字节为0）生成chunk编码
    if (m_remaining_bytes == 0) {
        m_buffer = Chunk::toChunk(data, is_last);
        m_remaining_bytes = m_buffer.size();
    }

    // 计算当前发送位置
    size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
    const char* send_ptr = m_buffer.data() + sent_bytes;

    return SendResponseAwaitable(*this, m_socket.send(send_ptr, m_remaining_bytes));
}

} // namespace galay::http
