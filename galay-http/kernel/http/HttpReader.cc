#include "HttpReader.h"
#include "HttpLog.h"
#include "galay-http/protoc/http/HttpChunk.h"

namespace galay::http
{

std::expected<bool, HttpError> GetRequestAwaitable::await_resume()
{
    // 1. 获取ReadvAwaitable的结果
    auto readv_result = m_readv_awaitable.await_resume();
    if (!readv_result) {
        // IO错误
        HTTP_LOG_DEBUG("readv failed: {}", readv_result.error().message());
        return std::unexpected(HttpError(kRecvError, readv_result.error().message()));
    }

    ssize_t bytes_read = readv_result.value();

    // 连接关闭
    if (bytes_read == 0) {
        HTTP_LOG_DEBUG("connection closed by peer");
        return std::unexpected(HttpError(kConnectionClose));
    }

    // 2. 将读取的数据produce到RingBuffer
    m_ring_buffer.produce(bytes_read);
    m_total_received += bytes_read;

    HTTP_LOG_DEBUG("received {} bytes, total: {}, readable: {}",
                 bytes_read, m_total_received, m_ring_buffer.readable());

    // 3. 获取RingBuffer的可读iovec
    auto read_iovecs = m_ring_buffer.getReadIovecs();
    if (read_iovecs.empty()) {
        // 没有可读数据，返回不完整
        return false;
    }

    // 4. 调用HttpRequest.fromIOVec进行解析
    auto [error_code, consumed] = m_request.fromIOVec(read_iovecs);

    // 5. 根据fromIOVec返回值判断消费RingBuffer多少数据
    if (consumed > 0) {
        m_ring_buffer.consume(consumed);
        HTTP_LOG_DEBUG("consumed {} bytes from ring buffer", consumed);
    }

    // 6. 检查是否达到最大header长度但头部仍未完整
    if (error_code == kHeaderInComplete || error_code == kIncomplete) {
        if (m_total_received >= m_setting.getMaxHeaderSize() && !m_request.isComplete()) {
            HTTP_LOG_DEBUG("header too large: received {} bytes, max: {}",
                        m_total_received, m_setting.getMaxHeaderSize());
            return std::unexpected(HttpError(kHeaderTooLarge));
        }
        // 数据不完整，返回false，用户继续调用
        return false;
    }

    // 7. 检查其他错误
    if (error_code != kNoError) {
        HTTP_LOG_DEBUG("parse error: {}", static_cast<int>(error_code));
        return std::unexpected(HttpError(error_code));
    }

    // 8. 检查是否完整解析
    if (m_request.isComplete()) {
        HTTP_LOG_DEBUG("request parsing completed");
        return true;
    }

    // 未完整（包括头未完整和body未完整，chunk只需要头完整）
    return false;
}

std::expected<bool, HttpError> GetResponseAwaitable::await_resume()
{
    // 1. 获取ReadvAwaitable的结果
    auto readv_result = m_readv_awaitable.await_resume();
    if (!readv_result) {
        // IO错误
        HTTP_LOG_DEBUG("readv failed: {}", readv_result.error().message());
        return std::unexpected(HttpError(kRecvError, readv_result.error().message()));
    }

    ssize_t bytes_read = readv_result.value();

    // 连接关闭
    if (bytes_read == 0) {
        HTTP_LOG_DEBUG("connection closed by peer");
        return std::unexpected(HttpError(kConnectionClose));
    }

    // 2. 将读取的数据produce到RingBuffer
    m_ring_buffer.produce(bytes_read);
    m_total_received += bytes_read;

    HTTP_LOG_DEBUG("received {} bytes, total: {}, readable: {}",
                 bytes_read, m_total_received, m_ring_buffer.readable());

    // 3. 获取RingBuffer的可读iovec
    auto read_iovecs = m_ring_buffer.getReadIovecs();
    if (read_iovecs.empty()) {
        // 没有可读数据，返回不完整
        return false;
    }

    // 4. 调用HttpResponse.fromIOVec进行解析
    auto [error_code, consumed] = m_response.fromIOVec(read_iovecs);

    // 5. 根据fromIOVec返回值判断消费RingBuffer多少数据
    if (consumed > 0) {
        m_ring_buffer.consume(consumed);
        HTTP_LOG_DEBUG("consumed {} bytes from ring buffer", consumed);
    }

    // 6. 检查是否达到最大header长度但头部仍未完整
    if (error_code == kHeaderInComplete || error_code == kIncomplete) {
        if (m_total_received >= m_setting.getMaxHeaderSize() && !m_response.isComplete()) {
            HTTP_LOG_DEBUG("header too large: received {} bytes, max: {}",
                        m_total_received, m_setting.getMaxHeaderSize());
            return std::unexpected(HttpError(kHeaderTooLarge));
        }
        // 数据不完整，返回false，用户继续调用
        return false;
    }

    // 7. 检查其他错误
    if (error_code != kNoError) {
        HTTP_LOG_DEBUG("parse error: {}", static_cast<int>(error_code));
        return std::unexpected(HttpError(error_code));
    }

    // 8. 检查是否完整解析
    if (m_response.isComplete()) {
        HTTP_LOG_DEBUG("response parsing completed");
        return true;
    }

    // 未完整（包括头未完整和body未完整，chunk只需要头完整）
    return false;
}

std::expected<bool, HttpError> GetChunkAwaitable::await_resume()
{
    // 1. 获取ReadvAwaitable的结果
    auto readv_result = m_readv_awaitable.await_resume();
    if (!readv_result) {
        // IO错误
        HTTP_LOG_DEBUG("readv failed: {}", readv_result.error().message());
        return std::unexpected(HttpError(kRecvError, readv_result.error().message()));
    }

    ssize_t bytes_read = readv_result.value();

    // 连接关闭
    if (bytes_read == 0) {
        HTTP_LOG_DEBUG("connection closed by peer");
        return std::unexpected(HttpError(kConnectionClose));
    }

    // 2. 将读取的数据produce到RingBuffer
    m_ring_buffer.produce(bytes_read);
    HTTP_LOG_DEBUG("received {} bytes, readable: {}", bytes_read, m_ring_buffer.readable());

    // 3. 获取RingBuffer的可读iovec
    auto read_iovecs = m_ring_buffer.getReadIovecs();
    if (read_iovecs.empty()) {
        // 没有可读数据，返回不完整
        return false;
    }

    // 4. 使用Chunk类解析chunk数据
    auto result = Chunk::fromIOVec(read_iovecs, m_chunk_data);

    if (!result) {
        // 解析错误
        auto& error = result.error();
        if (error.code() == kIncomplete) {
            // 数据不完整，需要继续调用
            HTTP_LOG_DEBUG("chunk data incomplete, need more data");
            return false;
        }
        // 其他错误
        HTTP_LOG_DEBUG("chunk parse error: {}", error.message());
        return std::unexpected(error);
    }

    // 5. 解析成功，消费RingBuffer
    auto [is_last, consumed] = result.value();
    m_ring_buffer.consume(consumed);
    HTTP_LOG_DEBUG("consumed {} bytes from ring buffer, is_last: {}", consumed, is_last);

    // 6. 返回结果
    if (is_last) {
        HTTP_LOG_DEBUG("chunk transfer complete");
        return true;
    }

    // 还有更多chunk
    return false;
}

} // namespace galay::http
