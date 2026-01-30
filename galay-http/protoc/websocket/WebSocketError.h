#ifndef GALAY_WEBSOCKET_ERROR_H
#define GALAY_WEBSOCKET_ERROR_H

#include "WebSocketBase.h"
#include <string>

namespace galay::websocket
{

/**
 * @brief WebSocket 错误码
 */
enum WsErrorCode
{
    kWsNoError = 0,              // 无错误
    kWsIncomplete,               // 数据不完整
    kWsInvalidFrame,             // 无效的帧
    kWsInvalidOpcode,            // 无效的操作码
    kWsInvalidPayloadLength,     // 无效的payload长度
    kWsControlFrameTooLarge,     // 控制帧过大（>125字节）
    kWsControlFrameFragmented,   // 控制帧不能分片
    kWsInvalidUtf8,              // 无效的UTF-8编码
    kWsProtocolError,            // 协议错误
    kWsConnectionClosed,         // 连接已关闭
    kWsMessageTooLarge,          // 消息过大
    kWsInvalidCloseCode,         // 无效的关闭码
    kWsReservedBitsSet,          // 保留位被设置
    kWsMaskRequired,             // 需要掩码（客户端->服务器）
    kWsMaskNotAllowed,           // 不允许掩码（服务器->客户端）
    kWsConnectionError,          // 连接错误
    kWsSendError,                // 发送错误
    kWsUpgradeFailed,            // 升级失败
    kWsUnknownError              // 未知错误
};

/**
 * @brief WebSocket 错误类
 */
class WsError
{
public:
    WsError(WsErrorCode code, const std::string& extra_msg = "")
        : m_code(code)
        , m_extra_msg(extra_msg)
    {
    }

    WsErrorCode code() const { return m_code; }

    std::string message() const
    {
        std::string msg = getErrorMessage(m_code);
        if (!m_extra_msg.empty()) {
            msg += ": " + m_extra_msg;
        }
        return msg;
    }

    /**
     * @brief 转换为WebSocket关闭状态码
     */
    WsCloseCode toCloseCode() const
    {
        switch (m_code) {
            case kWsInvalidFrame:
            case kWsInvalidOpcode:
            case kWsControlFrameTooLarge:
            case kWsControlFrameFragmented:
            case kWsReservedBitsSet:
            case kWsMaskRequired:
            case kWsMaskNotAllowed:
                return WsCloseCode::ProtocolError;

            case kWsInvalidUtf8:
            case kWsInvalidPayloadLength:
                return WsCloseCode::InvalidPayload;

            case kWsMessageTooLarge:
                return WsCloseCode::MessageTooBig;

            case kWsProtocolError:
                return WsCloseCode::ProtocolError;

            default:
                return WsCloseCode::InternalError;
        }
    }

private:
    static std::string getErrorMessage(WsErrorCode code)
    {
        switch (code) {
            case kWsNoError:
                return "No error";
            case kWsIncomplete:
                return "Data incomplete";
            case kWsInvalidFrame:
                return "Invalid frame";
            case kWsInvalidOpcode:
                return "Invalid opcode";
            case kWsInvalidPayloadLength:
                return "Invalid payload length";
            case kWsControlFrameTooLarge:
                return "Control frame too large (>125 bytes)";
            case kWsControlFrameFragmented:
                return "Control frame cannot be fragmented";
            case kWsInvalidUtf8:
                return "Invalid UTF-8 encoding";
            case kWsProtocolError:
                return "Protocol error";
            case kWsConnectionClosed:
                return "Connection closed";
            case kWsMessageTooLarge:
                return "Message too large";
            case kWsInvalidCloseCode:
                return "Invalid close code";
            case kWsReservedBitsSet:
                return "Reserved bits are set";
            case kWsMaskRequired:
                return "Mask required (client to server)";
            case kWsMaskNotAllowed:
                return "Mask not allowed (server to client)";
            case kWsConnectionError:
                return "Connection error";
            case kWsSendError:
                return "Send error";
            case kWsUpgradeFailed:
                return "WebSocket upgrade failed";
            case kWsUnknownError:
                return "Unknown error";
            default:
                return "Unknown error code";
        }
    }

private:
    WsErrorCode m_code;
    std::string m_extra_msg;
};

} // namespace galay::websocket

#endif // GALAY_WEBSOCKET_ERROR_H
