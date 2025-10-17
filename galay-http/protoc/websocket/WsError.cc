#include "WsError.h"

namespace galay::http
{
    WsError::WsError(WsErrorCode code) : m_code(code) {}

    WsErrorCode WsError::code() const
    {
        return m_code;
    }

    std::string WsError::message() const
    {
        switch (m_code) {
            case kWsError_NoError:
                return "No error";
            case kWsError_ConnectionClose:
                return "Connection closed";
            case kWsError_TcpRecvError:
                return "TCP receive error";
            case kWsError_TcpSendError:
                return "TCP send error";
            case kWsError_RecvTimeOut:
                return "Receive timeout";
            case kWsError_SendTimeOut:
                return "Send timeout";
            case kWsError_InvalidFrame:
                return "Invalid WebSocket frame";
            case kWsError_InvalidOpcode:
                return "Invalid opcode";
            case kWsError_FrameTooLarge:
                return "Frame too large";
            case kWsError_InvalidMask:
                return "Invalid mask";
            case kWsError_ProtocolError:
                return "WebSocket protocol error";
            case kWsError_InvalidUTF8:
                return "Invalid UTF-8 encoding";
            case kWsError_MessageTooLarge:
                return "Message too large";
            case kWsError_UnexpectedContinuation:
                return "Unexpected continuation frame";
            case kWsError_FragmentedControl:
                return "Fragmented control frame";
            case kWsError_ReservedBitSet:
                return "Reserved bit is set";
            case kWsError_CloseFrameInvalid:
                return "Invalid close frame";
            case kWsError_PingTimeOut:
                return "Ping timeout";
            default:
                return "Unknown error";
        }
    }

    WsCloseCode WsError::toWsCloseCode() const
    {
        switch (m_code) {
            case kWsError_NoError:
                return WsCloseCode::Normal;
            case kWsError_InvalidFrame:
            case kWsError_InvalidOpcode:
            case kWsError_InvalidMask:
            case kWsError_ProtocolError:
            case kWsError_UnexpectedContinuation:
            case kWsError_FragmentedControl:
            case kWsError_ReservedBitSet:
                return WsCloseCode::ProtocolError;
            case kWsError_InvalidUTF8:
            case kWsError_CloseFrameInvalid:
                return WsCloseCode::InvalidPayload;
            case kWsError_FrameTooLarge:
            case kWsError_MessageTooLarge:
                return WsCloseCode::MessageTooBig;
            default:
                return WsCloseCode::InternalError;
        }
    }
}

