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

} // namespace galay::http2

#endif // GALAY_HTTP2_ERROR_H
