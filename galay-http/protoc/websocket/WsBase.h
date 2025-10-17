#ifndef GALAY_WS_BASE_H
#define GALAY_WS_BASE_H

#include <cstdint>
#include <string>
#include <galay/common/Base.h>
#include <galay/common/Log.h>

namespace galay::http 
{
    // WebSocket 默认配置
    #define DEFAULT_WS_RECV_TIMEOUT         std::chrono::milliseconds(30000)
    #define DEFAULT_WS_SEND_TIMEOUT         std::chrono::milliseconds(30000)
    #define DEFAULT_WS_MAX_FRAME_SIZE       (10 * 1024 * 1024)  // 10MB
    #define DEFAULT_WS_PING_INTERVAL        std::chrono::seconds(30)
    #define DEFAULT_WS_PONG_TIMEOUT         std::chrono::seconds(10)

    // WebSocket 魔术字符串（用于握手）
    #define WS_MAGIC_STRING "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

    // WebSocket 操作码 (Opcode)
    enum class WsOpcode : uint8_t
    {
        Continuation = 0x0,  // 续传帧
        Text = 0x1,          // 文本帧
        Binary = 0x2,        // 二进制帧
        // 0x3-0x7 保留用于未来的非控制帧
        Close = 0x8,         // 关闭帧
        Ping = 0x9,          // Ping 帧
        Pong = 0xA,          // Pong 帧
        // 0xB-0xF 保留用于未来的控制帧
        Unknown = 0xFF
    };

    // WebSocket 关闭状态码
    enum class WsCloseCode : uint16_t
    {
        Normal = 1000,              // 正常关闭
        GoingAway = 1001,           // 端点离开（如服务器关闭或浏览器导航离开）
        ProtocolError = 1002,       // 协议错误
        UnsupportedData = 1003,     // 接收到不支持的数据类型
        NoStatusReceived = 1005,    // 保留，不应在 Close 帧中使用
        AbnormalClosure = 1006,     // 保留，不应在 Close 帧中使用
        InvalidPayload = 1007,      // 接收到不一致的消息（如非 UTF-8 文本）
        PolicyViolation = 1008,     // 接收到违反策略的消息
        MessageTooBig = 1009,       // 接收到过大的消息
        MandatoryExtension = 1010,  // 客户端期望服务器协商一个或多个扩展
        InternalError = 1011,       // 服务器遇到意外情况
        ServiceRestart = 1012,      // 服务器重启
        TryAgainLater = 1013,       // 服务器临时过载
        BadGateway = 1014,          // 网关或代理错误
        TLSHandshake = 1015         // 保留，TLS 握手失败
    };

    // WebSocket 帧类型
    enum class WsFrameType
    {
        Text,           // 文本消息
        Binary,         // 二进制消息
        Close,          // 关闭连接
        Ping,           // Ping
        Pong,           // Pong
        Continuation,   // 续传帧
        Unknown
    };

    // 辅助函数
    inline std::string wsOpcodeToString(WsOpcode opcode)
    {
        switch (opcode) {
            case WsOpcode::Continuation: return "Continuation";
            case WsOpcode::Text: return "Text";
            case WsOpcode::Binary: return "Binary";
            case WsOpcode::Close: return "Close";
            case WsOpcode::Ping: return "Ping";
            case WsOpcode::Pong: return "Pong";
            default: return "Unknown";
        }
    }

    inline std::string wsCloseCodeToString(WsCloseCode code)
    {
        switch (code) {
            case WsCloseCode::Normal: return "Normal Closure";
            case WsCloseCode::GoingAway: return "Going Away";
            case WsCloseCode::ProtocolError: return "Protocol Error";
            case WsCloseCode::UnsupportedData: return "Unsupported Data";
            case WsCloseCode::InvalidPayload: return "Invalid Payload";
            case WsCloseCode::PolicyViolation: return "Policy Violation";
            case WsCloseCode::MessageTooBig: return "Message Too Big";
            case WsCloseCode::InternalError: return "Internal Error";
            default: return "Unknown";
        }
    }

    inline bool isControlFrame(WsOpcode opcode)
    {
        return opcode == WsOpcode::Close || 
               opcode == WsOpcode::Ping || 
               opcode == WsOpcode::Pong;
    }

    inline bool isDataFrame(WsOpcode opcode)
    {
        return opcode == WsOpcode::Text || 
               opcode == WsOpcode::Binary || 
               opcode == WsOpcode::Continuation;
    }
}

#endif

