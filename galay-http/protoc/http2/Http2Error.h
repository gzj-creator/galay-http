#ifndef GALAY_HTTP2_ERROR_H
#define GALAY_HTTP2_ERROR_H

#include "Http2Base.h"
#include <string>
#include <expected>

namespace galay::http2
{

/**
 * @brief HTTP/2 错误类
 */
class Http2Error
{
public:
    Http2Error() = default;

    Http2Error(Http2ErrorCode code, std::string message = "")
        : m_code(code), m_message(std::move(message)) {}

    Http2ErrorCode code() const { return m_code; }
    const std::string& message() const { return m_message; }

    bool isError() const { return m_code != Http2ErrorCode::NoError; }

    std::string toString() const {
        return http2ErrorCodeToString(m_code) + (m_message.empty() ? "" : ": " + m_message);
    }

    // 静态工厂方法
    static Http2Error noError() { return Http2Error(Http2ErrorCode::NoError); }
    static Http2Error protocolError(const std::string& msg = "") { return Http2Error(Http2ErrorCode::ProtocolError, msg); }
    static Http2Error internalError(const std::string& msg = "") { return Http2Error(Http2ErrorCode::InternalError, msg); }
    static Http2Error flowControlError(const std::string& msg = "") { return Http2Error(Http2ErrorCode::FlowControlError, msg); }
    static Http2Error frameSizeError(const std::string& msg = "") { return Http2Error(Http2ErrorCode::FrameSizeError, msg); }
    static Http2Error compressionError(const std::string& msg = "") { return Http2Error(Http2ErrorCode::CompressionError, msg); }

private:
    Http2ErrorCode m_code = Http2ErrorCode::NoError;
    std::string m_message;
};

/**
 * @brief 连接/流运行时错误分类
 * @details 用于区分连接级致命错误与流级可恢复错误。
 */
enum class Http2RuntimeError
{
    ProtocolViolation,
    FlowControlViolation,
    StreamClosed,
    StreamReset,
    Timeout,
    PeerClosed,
    IoError
};

inline std::string http2RuntimeErrorToString(Http2RuntimeError error) {
    switch (error) {
        case Http2RuntimeError::ProtocolViolation:    return "protocol-violation";
        case Http2RuntimeError::FlowControlViolation: return "flow-control-violation";
        case Http2RuntimeError::StreamClosed:         return "stream-closed";
        case Http2RuntimeError::StreamReset:          return "stream-reset";
        case Http2RuntimeError::Timeout:              return "timeout";
        case Http2RuntimeError::PeerClosed:           return "peer-closed";
        case Http2RuntimeError::IoError:              return "io-error";
    }
    return "unknown";
}

inline bool http2IsConnectionFatal(Http2RuntimeError error) {
    switch (error) {
        case Http2RuntimeError::ProtocolViolation:
        case Http2RuntimeError::FlowControlViolation:
            return true;
        case Http2RuntimeError::StreamClosed:
        case Http2RuntimeError::StreamReset:
        case Http2RuntimeError::Timeout:
        case Http2RuntimeError::PeerClosed:
        case Http2RuntimeError::IoError:
            return false;
    }
    return false;
}

/**
 * @brief GOAWAY 导致的请求拒绝错误
 * @details retryable=true 表示该 stream 未被服务端接收，可安全重试
 */
struct Http2GoAwayError
{
    uint32_t stream_id = 0;
    uint32_t last_stream_id = 0;
    Http2ErrorCode error_code = Http2ErrorCode::NoError;
    bool retryable = false;
    std::string debug;

    std::string toString() const {
        return "GOAWAY stream_id=" + std::to_string(stream_id) +
               " last_stream_id=" + std::to_string(last_stream_id) +
               " error=" + http2ErrorCodeToString(error_code) +
               " retryable=" + std::string(retryable ? "true" : "false") +
               (debug.empty() ? "" : (" debug=" + debug));
    }
};

} // namespace galay::http2

#endif // GALAY_HTTP2_ERROR_H
