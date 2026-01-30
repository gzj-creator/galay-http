#ifndef GALAY_HTTP2_BASE_H
#define GALAY_HTTP2_BASE_H

#include <cstdint>
#include <string>
#include <string_view>

namespace galay::http2
{

// HTTP/2 连接前言 (Connection Preface)
// 客户端必须在连接开始时发送此字符串
constexpr std::string_view kHttp2ConnectionPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
constexpr size_t kHttp2ConnectionPrefaceLength = 24;

// HTTP/2 帧头长度固定为 9 字节
constexpr size_t kHttp2FrameHeaderLength = 9;

// HTTP/2 默认设置
constexpr uint32_t kDefaultHeaderTableSize = 4096;
constexpr uint32_t kDefaultEnablePush = 1;
constexpr uint32_t kDefaultMaxConcurrentStreams = 100;
constexpr uint32_t kDefaultInitialWindowSize = 65535;
constexpr uint32_t kDefaultMaxFrameSize = 16384;
constexpr uint32_t kDefaultMaxHeaderListSize = 8192;

// HTTP/2 帧大小限制
constexpr uint32_t kMinFrameSize = 16384;      // 2^14
constexpr uint32_t kMaxFrameSize = 16777215;   // 2^24 - 1

// HTTP/2 流 ID 限制
constexpr uint32_t kMaxStreamId = 0x7FFFFFFF;  // 2^31 - 1

// HTTP/2 帧类型
enum class Http2FrameType : uint8_t
{
    Data = 0x0,
    Headers = 0x1,
    Priority = 0x2,
    RstStream = 0x3,
    Settings = 0x4,
    PushPromise = 0x5,
    Ping = 0x6,
    GoAway = 0x7,
    WindowUpdate = 0x8,
    Continuation = 0x9,
    Unknown = 0xFF
};

// HTTP/2 帧标志
namespace Http2FrameFlags
{
    // DATA 帧标志
    constexpr uint8_t kEndStream = 0x1;
    constexpr uint8_t kPadded = 0x8;

    // HEADERS 帧标志
    constexpr uint8_t kEndHeaders = 0x4;
    constexpr uint8_t kPriority = 0x20;

    // SETTINGS 帧标志
    constexpr uint8_t kAck = 0x1;

    // PING 帧标志
    // kAck = 0x1
}

// HTTP/2 设置参数标识符
enum class Http2SettingsId : uint16_t
{
    HeaderTableSize = 0x1,
    EnablePush = 0x2,
    MaxConcurrentStreams = 0x3,
    InitialWindowSize = 0x4,
    MaxFrameSize = 0x5,
    MaxHeaderListSize = 0x6
};

// HTTP/2 错误码
enum class Http2ErrorCode : uint32_t
{
    NoError = 0x0,
    ProtocolError = 0x1,
    InternalError = 0x2,
    FlowControlError = 0x3,
    SettingsTimeout = 0x4,
    StreamClosed = 0x5,
    FrameSizeError = 0x6,
    RefusedStream = 0x7,
    Cancel = 0x8,
    CompressionError = 0x9,
    ConnectError = 0xa,
    EnhanceYourCalm = 0xb,
    InadequateSecurity = 0xc,
    Http11Required = 0xd
};

// HTTP/2 流状态
enum class Http2StreamState
{
    Idle,
    ReservedLocal,
    ReservedRemote,
    Open,
    HalfClosedLocal,
    HalfClosedRemote,
    Closed
};

// 帧类型转字符串
std::string http2FrameTypeToString(Http2FrameType type);

// 错误码转字符串
std::string http2ErrorCodeToString(Http2ErrorCode code);

// 流状态转字符串
std::string http2StreamStateToString(Http2StreamState state);

} // namespace galay::http2

#endif // GALAY_HTTP2_BASE_H
