#include "Http2Error.h"

namespace galay::http
{
    std::string Http2Error::message() const
    {
        std::string msg;
        switch (m_type) {
            case kHttp2Error_Success:
                msg = "Success";
                break;
            case kHttp2Error_ConnectionClosed:
                msg = "Connection closed";
                break;
            case kHttp2Error_ConnectionTimeout:
                msg = "Connection timeout";
                break;
            case kHttp2Error_InvalidPreface:
                msg = "Invalid connection preface";
                break;
            case kHttp2Error_GoAway:
                msg = "Received GOAWAY";
                break;
            case kHttp2Error_InvalidFrameSize:
                msg = "Invalid frame size";
                break;
            case kHttp2Error_InvalidFrameType:
                msg = "Invalid frame type";
                break;
            case kHttp2Error_FrameTooLarge:
                msg = "Frame too large";
                break;
            case kHttp2Error_ProtocolError:
                msg = "Protocol error";
                break;
            case kHttp2Error_StreamClosed:
                msg = "Stream closed";
                break;
            case kHttp2Error_StreamNotFound:
                msg = "Stream not found";
                break;
            case kHttp2Error_TooManyStreams:
                msg = "Too many streams";
                break;
            case kHttp2Error_StreamIdInvalid:
                msg = "Invalid stream ID";
                break;
            case kHttp2Error_FlowControlError:
                msg = "Flow control error";
                break;
            case kHttp2Error_WindowSizeExceeded:
                msg = "Window size exceeded";
                break;
            case kHttp2Error_InvalidSettings:
                msg = "Invalid settings";
                break;
            case kHttp2Error_SettingsTimeout:
                msg = "Settings timeout";
                break;
            case kHttp2Error_CompressionError:
                msg = "Compression error";
                break;
            case kHttp2Error_HeadersTooLarge:
                msg = "Headers too large";
                break;
            case kHttp2Error_InternalError:
                msg = "Internal error";
                break;
            case kHttp2Error_SendError:
                msg = "Send error";
                break;
            case kHttp2Error_RecvError:
                msg = "Receive error";
                break;
            case kHttp2Error_Cancelled:
                msg = "Cancelled";
                break;
            default:
                msg = "Unknown error";
                break;
        }
        
        if (!m_detail.empty()) {
            msg += ": " + m_detail;
        }
        
        return msg;
    }
    
    Http2ErrorCode Http2Error::toHttp2ErrorCode() const
    {
        switch (m_type) {
            case kHttp2Error_Success:
                return Http2ErrorCode::NO_ERROR;
            case kHttp2Error_ProtocolError:
            case kHttp2Error_InvalidFrameSize:
            case kHttp2Error_InvalidFrameType:
            case kHttp2Error_InvalidPreface:
            case kHttp2Error_StreamIdInvalid:
                return Http2ErrorCode::PROTOCOL_ERROR;
            case kHttp2Error_InternalError:
            case kHttp2Error_SendError:
            case kHttp2Error_RecvError:
                return Http2ErrorCode::INTERNAL_ERROR;
            case kHttp2Error_FlowControlError:
            case kHttp2Error_WindowSizeExceeded:
                return Http2ErrorCode::FLOW_CONTROL_ERROR;
            case kHttp2Error_SettingsTimeout:
                return Http2ErrorCode::SETTINGS_TIMEOUT;
            case kHttp2Error_StreamClosed:
                return Http2ErrorCode::STREAM_CLOSED;
            case kHttp2Error_FrameTooLarge:
                return Http2ErrorCode::FRAME_SIZE_ERROR;
            case kHttp2Error_TooManyStreams:
                return Http2ErrorCode::REFUSED_STREAM;
            case kHttp2Error_Cancelled:
                return Http2ErrorCode::CANCEL;
            case kHttp2Error_CompressionError:
                return Http2ErrorCode::COMPRESSION_ERROR;
            default:
                return Http2ErrorCode::INTERNAL_ERROR;
        }
    }
    
    bool Http2Error::isConnectionError() const
    {
        switch (m_type) {
            case kHttp2Error_ConnectionClosed:
            case kHttp2Error_ConnectionTimeout:
            case kHttp2Error_InvalidPreface:
            case kHttp2Error_GoAway:
            case kHttp2Error_ProtocolError:
            case kHttp2Error_InvalidSettings:
            case kHttp2Error_SettingsTimeout:
            case kHttp2Error_CompressionError:
                return true;
            default:
                return false;
        }
    }
    
    bool Http2Error::isStreamError() const
    {
        switch (m_type) {
            case kHttp2Error_StreamClosed:
            case kHttp2Error_StreamNotFound:
            case kHttp2Error_StreamIdInvalid:
            case kHttp2Error_FlowControlError:
            case kHttp2Error_WindowSizeExceeded:
            case kHttp2Error_HeadersTooLarge:
            case kHttp2Error_Cancelled:
                return true;
            default:
                return false;
        }
    }
}

