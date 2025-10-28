#ifndef GALAY_HTTP2_BASE_H
#define GALAY_HTTP2_BASE_H

#include <cstdint>
#include <string>
#include <chrono>

namespace galay::http
{
    // HTTP/2 默认配置
    #define DEFAULT_HTTP2_RECV_TIMEOUT          std::chrono::milliseconds(30000)
    #define DEFAULT_HTTP2_SEND_TIMEOUT          std::chrono::milliseconds(30000)
    #define DEFAULT_HTTP2_MAX_FRAME_SIZE        16384       // 16KB (RFC 7540 规定的默认值)
    #define DEFAULT_HTTP2_MAX_HEADER_LIST_SIZE  (8192)      // 8KB
    #define DEFAULT_HTTP2_INITIAL_WINDOW_SIZE   65535       // 64KB - 1
    #define DEFAULT_HTTP2_MAX_CONCURRENT_STREAMS 100
    
    // HTTP/2 连接前言 (Connection Preface)
    const char HTTP2_CONNECTION_PREFACE[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    const size_t HTTP2_CONNECTION_PREFACE_LENGTH = 24;
    
    // HTTP/2 帧头长度
    const size_t HTTP2_FRAME_HEADER_SIZE = 9;
    
    /**
     * @brief HTTP/2 帧类型
     * 
     * 定义了 HTTP/2 协议中所有的帧类型
     * 参考：RFC 7540 Section 6
     */
    enum class Http2FrameType : uint8_t
    {
        DATA          = 0x0,  // 数据帧
        HEADERS       = 0x1,  // 头部帧
        PRIORITY      = 0x2,  // 优先级帧
        RST_STREAM    = 0x3,  // 流重置帧
        SETTINGS      = 0x4,  // 设置帧
        PUSH_PROMISE  = 0x5,  // 推送承诺帧
        PING          = 0x6,  // Ping 帧
        GOAWAY        = 0x7,  // GoAway 帧（关闭连接）
        WINDOW_UPDATE = 0x8,  // 窗口更新帧
        CONTINUATION  = 0x9,  // 延续帧（用于分片的头部）
        HTTP2_UNKNOWN = 0xFF  // 未知类型
    };
    
    /**
     * @brief HTTP/2 帧标志位
     * 
     * 不同类型的帧可以使用不同的标志位
     */
    enum Http2FrameFlags : uint8_t
    {
        FLAG_NONE         = 0x0,
        FLAG_END_STREAM   = 0x1,   // 流结束（DATA, HEADERS）
        FLAG_ACK          = 0x1,   // 确认（SETTINGS, PING）
        FLAG_END_HEADERS  = 0x4,   // 头部结束（HEADERS, PUSH_PROMISE, CONTINUATION）
        FLAG_PADDED       = 0x8,   // 有填充（DATA, HEADERS, PUSH_PROMISE）
        FLAG_PRIORITY     = 0x20   // 有优先级信息（HEADERS）
    };
    
    /**
     * @brief HTTP/2 错误码
     * 
     * 用于 RST_STREAM 和 GOAWAY 帧
     * 参考：RFC 7540 Section 7
     */
    enum class Http2ErrorCode : uint32_t
    {
        NO_ERROR            = 0x0,  // 无错误
        PROTOCOL_ERROR      = 0x1,  // 协议错误
        INTERNAL_ERROR      = 0x2,  // 内部错误
        FLOW_CONTROL_ERROR  = 0x3,  // 流控错误
        SETTINGS_TIMEOUT    = 0x4,  // 设置超时
        STREAM_CLOSED       = 0x5,  // 流已关闭
        FRAME_SIZE_ERROR    = 0x6,  // 帧大小错误
        REFUSED_STREAM      = 0x7,  // 拒绝流
        CANCEL              = 0x8,  // 取消
        COMPRESSION_ERROR   = 0x9,  // 压缩错误
        CONNECT_ERROR       = 0xA,  // 连接错误
        ENHANCE_YOUR_CALM   = 0xB,  // 请求过于频繁
        INADEQUATE_SECURITY = 0xC,  // 安全性不足
        HTTP_1_1_REQUIRED   = 0xD   // 需要 HTTP/1.1
    };
    
    /**
     * @brief HTTP/2 设置参数
     * 
     * 用于 SETTINGS 帧
     * 参考：RFC 7540 Section 6.5.2
     */
    enum class Http2SettingsId : uint16_t
    {
        HEADER_TABLE_SIZE      = 0x1,  // HPACK 头部表大小
        ENABLE_PUSH            = 0x2,  // 启用服务器推送
        MAX_CONCURRENT_STREAMS = 0x3,  // 最大并发流数
        INITIAL_WINDOW_SIZE    = 0x4,  // 初始窗口大小
        MAX_FRAME_SIZE         = 0x5,  // 最大帧大小
        MAX_HEADER_LIST_SIZE   = 0x6   // 最大头部列表大小
    };
    
    /**
     * @brief HTTP/2 流状态
     * 
     * 参考：RFC 7540 Section 5.1
     */
    enum class Http2StreamState
    {
        IDLE,           // 空闲
        RESERVED_LOCAL, // 本地保留
        RESERVED_REMOTE,// 远程保留
        OPEN,           // 打开
        HALF_CLOSED_LOCAL,   // 本地半关闭
        HALF_CLOSED_REMOTE,  // 远程半关闭
        CLOSED          // 关闭
    };
    
    /**
     * @brief 判断是否是控制帧（连接级别的帧）
     */
    inline bool isConnectionFrame(Http2FrameType type)
    {
        return type == Http2FrameType::SETTINGS ||
               type == Http2FrameType::PING ||
               type == Http2FrameType::GOAWAY ||
               type == Http2FrameType::WINDOW_UPDATE;
    }
    
    /**
     * @brief 将帧类型转换为字符串
     */
    inline std::string http2FrameTypeToString(Http2FrameType type)
    {
        switch (type) {
            case Http2FrameType::DATA:          return "DATA";
            case Http2FrameType::HEADERS:       return "HEADERS";
            case Http2FrameType::PRIORITY:      return "PRIORITY";
            case Http2FrameType::RST_STREAM:    return "RST_STREAM";
            case Http2FrameType::SETTINGS:      return "SETTINGS";
            case Http2FrameType::PUSH_PROMISE:  return "PUSH_PROMISE";
            case Http2FrameType::PING:          return "PING";
            case Http2FrameType::GOAWAY:        return "GOAWAY";
            case Http2FrameType::WINDOW_UPDATE: return "WINDOW_UPDATE";
            case Http2FrameType::CONTINUATION:  return "CONTINUATION";
            default:                            return "UNKNOWN";
        }
    }
    
    /**
     * @brief 将错误码转换为字符串
     */
    inline std::string http2ErrorCodeToString(Http2ErrorCode code)
    {
        switch (code) {
            case Http2ErrorCode::NO_ERROR:            return "NO_ERROR";
            case Http2ErrorCode::PROTOCOL_ERROR:      return "PROTOCOL_ERROR";
            case Http2ErrorCode::INTERNAL_ERROR:      return "INTERNAL_ERROR";
            case Http2ErrorCode::FLOW_CONTROL_ERROR:  return "FLOW_CONTROL_ERROR";
            case Http2ErrorCode::SETTINGS_TIMEOUT:    return "SETTINGS_TIMEOUT";
            case Http2ErrorCode::STREAM_CLOSED:       return "STREAM_CLOSED";
            case Http2ErrorCode::FRAME_SIZE_ERROR:    return "FRAME_SIZE_ERROR";
            case Http2ErrorCode::REFUSED_STREAM:      return "REFUSED_STREAM";
            case Http2ErrorCode::CANCEL:              return "CANCEL";
            case Http2ErrorCode::COMPRESSION_ERROR:   return "COMPRESSION_ERROR";
            case Http2ErrorCode::CONNECT_ERROR:       return "CONNECT_ERROR";
            case Http2ErrorCode::ENHANCE_YOUR_CALM:   return "ENHANCE_YOUR_CALM";
            case Http2ErrorCode::INADEQUATE_SECURITY: return "INADEQUATE_SECURITY";
            case Http2ErrorCode::HTTP_1_1_REQUIRED:   return "HTTP_1_1_REQUIRED";
            default:                                  return "UNKNOWN";
        }
    }
    
    /**
     * @brief 将流状态转换为字符串
     */
    inline std::string http2StreamStateToString(Http2StreamState state)
    {
        switch (state) {
            case Http2StreamState::IDLE:                return "IDLE";
            case Http2StreamState::RESERVED_LOCAL:      return "RESERVED_LOCAL";
            case Http2StreamState::RESERVED_REMOTE:     return "RESERVED_REMOTE";
            case Http2StreamState::OPEN:                return "OPEN";
            case Http2StreamState::HALF_CLOSED_LOCAL:   return "HALF_CLOSED_LOCAL";
            case Http2StreamState::HALF_CLOSED_REMOTE:  return "HALF_CLOSED_REMOTE";
            case Http2StreamState::CLOSED:              return "CLOSED";
            default:                                    return "UNKNOWN";
        }
    }
}

#endif // GALAY_HTTP2_BASE_H

