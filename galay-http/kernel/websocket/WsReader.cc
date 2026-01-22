#include "WsReader.h"

namespace galay::websocket
{

std::expected<bool, WsError> GetFrameAwaitable::await_resume()
{
    auto result = m_readv_awaitable.await_resume();

    if (!result.has_value()) {
        // 读取失败
        return std::unexpected(WsError(kWsConnectionClosed, "Socket readv failed"));
    }

    size_t bytes_received = result.value();
    if (bytes_received == 0) {
        // 连接关闭
        return std::unexpected(WsError(kWsConnectionClosed, "Connection closed by peer"));
    }

    m_total_received += bytes_received;
    m_ring_buffer.produce(bytes_received);

    // 尝试解析帧
    auto iovecs = m_ring_buffer.getReadIovecs();
    auto parse_result = WsFrameParser::fromIOVec(iovecs, m_frame, m_is_server);

    if (!parse_result.has_value()) {
        WsError error = parse_result.error();

        // 如果是数据不完整，返回 false 继续读取
        if (error.code() == kWsIncomplete) {
            // 检查是否超过最大帧大小
            if (m_total_received > m_setting.max_frame_size) {
                return std::unexpected(WsError(kWsMessageTooLarge, "Frame size exceeds limit"));
            }
            return false;  // 需要继续读取
        }

        // 其他错误直接返回
        return std::unexpected(error);
    }

    // 解析成功，消费已读取的数据
    size_t consumed = parse_result.value();
    m_ring_buffer.consume(consumed);

    // 检查帧大小
    if (m_frame.header.payload_length > m_setting.max_frame_size) {
        return std::unexpected(WsError(kWsMessageTooLarge, "Frame payload too large"));
    }

    return true;  // 帧完整解析
}

std::expected<bool, WsError> GetMessageAwaitable::await_resume()
{
    auto result = m_readv_awaitable.await_resume();

    if (!result.has_value()) {
        return std::unexpected(WsError(kWsConnectionClosed, "Socket readv failed"));
    }

    size_t bytes_received = result.value();
    if (bytes_received == 0) {
        return std::unexpected(WsError(kWsConnectionClosed, "Connection closed by peer"));
    }

    m_total_received += bytes_received;
    m_ring_buffer.produce(bytes_received);

    // 循环解析帧，直到遇到 FIN=1 的帧
    while (true) {
        auto iovecs = m_ring_buffer.getReadIovecs();
        if (iovecs.empty()) {
            // 没有数据可读，需要继续接收
            return false;
        }

        WsFrame frame;
        auto parse_result = WsFrameParser::fromIOVec(iovecs, frame, m_is_server);

        if (!parse_result.has_value()) {
            WsError error = parse_result.error();

            if (error.code() == kWsIncomplete) {
                // 检查消息大小限制
                if (m_message.size() + m_total_received > m_setting.max_message_size) {
                    return std::unexpected(WsError(kWsMessageTooLarge, "Message size exceeds limit"));
                }
                return false;  // 需要继续读取
            }

            return std::unexpected(error);
        }

        // 解析成功，消费数据
        size_t consumed = parse_result.value();
        m_ring_buffer.consume(consumed);

        // 处理控制帧（Ping/Pong/Close）
        if (isControlFrame(frame.header.opcode)) {
            // 控制帧不应该被分片
            if (!frame.header.fin) {
                return std::unexpected(WsError(kWsControlFrameFragmented));
            }

            // 如果设置了控制帧回调，调用它
            if (m_control_frame_callback) {
                m_control_frame_callback(frame.header.opcode, frame.payload);
            }

            // 控制帧不累积到消息中，继续读取数据帧
            continue;
        }

        // 处理数据帧
        if (m_first_frame) {
            // 第一个帧，记录操作码
            if (frame.header.opcode == WsOpcode::Continuation) {
                return std::unexpected(WsError(kWsProtocolError, "First frame cannot be continuation"));
            }
            m_opcode = frame.header.opcode;
            m_first_frame = false;
        } else {
            // 后续帧必须是 Continuation
            if (frame.header.opcode != WsOpcode::Continuation) {
                return std::unexpected(WsError(kWsProtocolError, "Expected continuation frame"));
            }
        }

        // 累积消息内容
        m_message += frame.payload;

        // 检查消息大小
        if (m_message.size() > m_setting.max_message_size) {
            return std::unexpected(WsError(kWsMessageTooLarge, "Message size exceeds limit"));
        }

        // 如果是最后一个帧，返回 true
        if (frame.header.fin) {
            return true;
        }

        // 否则继续读取下一个帧
        // 如果 RingBuffer 中没有更多数据，返回 false 继续接收
        if (m_ring_buffer.getReadIovecs().empty()) {
            return false;
        }
    }
}

} // namespace galay::websocket
