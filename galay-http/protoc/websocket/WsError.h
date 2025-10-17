#ifndef GALAY_WS_ERROR_H
#define GALAY_WS_ERROR_H

#include <galay/common/Error.h>
#include "WsBase.h"

namespace galay::http
{
    enum WsErrorCode
    {
        kWsError_NoError = 0,               // 无错误
        kWsError_ConnectionClose,           // 连接已关闭
        kWsError_TcpRecvError,              // TCP 接收错误
        kWsError_TcpSendError,              // TCP 发送错误
        kWsError_RecvTimeOut,               // 接收超时
        kWsError_SendTimeOut,               // 发送超时
        kWsError_InvalidFrame,              // 无效的帧
        kWsError_InvalidOpcode,             // 无效的操作码
        kWsError_FrameTooLarge,             // 帧过大
        kWsError_InvalidMask,               // 无效的掩码
        kWsError_ProtocolError,             // 协议错误
        kWsError_InvalidUTF8,               // 无效的 UTF-8 编码
        kWsError_MessageTooLarge,           // 消息过大
        kWsError_UnexpectedContinuation,    // 意外的续传帧
        kWsError_FragmentedControl,         // 分片的控制帧（不允许）
        kWsError_ReservedBitSet,            // 保留位被设置
        kWsError_CloseFrameInvalid,         // 无效的关闭帧
        kWsError_PingTimeOut,               // Ping 超时
        kWsError_UnknownError               // 未知错误
    };

    class WsError
    {
    public:
        WsError(WsErrorCode code);
        WsErrorCode code() const;
        std::string message() const;
        WsCloseCode toWsCloseCode() const;
    private:
        WsErrorCode m_code;
    };
}

#endif

