#ifndef GALAY_HTTP2_ERROR_H
#define GALAY_HTTP2_ERROR_H

#include "Http2Base.h"
#include <string>

namespace galay::http
{
    /**
     * @brief HTTP/2 错误类型枚举
     */
    enum Http2ErrorType
    {
        kHttp2Error_Success = 0,
        
        // 连接错误
        kHttp2Error_ConnectionClosed,       // 连接已关闭
        kHttp2Error_ConnectionTimeout,      // 连接超时
        kHttp2Error_InvalidPreface,         // 无效的连接前言
        kHttp2Error_GoAway,                 // 收到 GOAWAY
        
        // 帧错误
        kHttp2Error_InvalidFrameSize,       // 无效的帧大小
        kHttp2Error_InvalidFrameType,       // 无效的帧类型
        kHttp2Error_FrameTooLarge,          // 帧过大
        kHttp2Error_ProtocolError,          // 协议错误
        
        // 流错误
        kHttp2Error_StreamClosed,           // 流已关闭
        kHttp2Error_StreamNotFound,         // 流不存在
        kHttp2Error_TooManyStreams,         // 流过多
        kHttp2Error_StreamIdInvalid,        // 无效的流 ID
        
        // 流控错误
        kHttp2Error_FlowControlError,       // 流控错误
        kHttp2Error_WindowSizeExceeded,     // 窗口大小超限
        
        // 设置错误
        kHttp2Error_InvalidSettings,        // 无效的设置
        kHttp2Error_SettingsTimeout,        // 设置超时
        
        // 头部压缩错误
        kHttp2Error_CompressionError,       // 压缩错误
        kHttp2Error_HeadersTooLarge,        // 头部过大
        
        // 其他错误
        kHttp2Error_InternalError,          // 内部错误
        kHttp2Error_SendError,              // 发送错误
        kHttp2Error_SendTimeout,            // 发送超时
        kHttp2Error_RecvError,              // 接收错误
        kHttp2Error_Cancelled               // 已取消
    };
    
    /**
     * @brief HTTP/2 错误类
     */
    class Http2Error
    {
    public:
        Http2Error() : m_type(kHttp2Error_Success) {}
        Http2Error(Http2ErrorType type) : m_type(type) {}
        Http2Error(Http2ErrorType type, const std::string& detail)
            : m_type(type), m_detail(detail) {}
        
        Http2ErrorType type() const { return m_type; }
        std::string message() const;
        std::string detail() const { return m_detail; }
        
        // 转换为 HTTP/2 协议错误码
        Http2ErrorCode toHttp2ErrorCode() const;
        
        // 判断是否是连接级别的错误（需要关闭整个连接）
        bool isConnectionError() const;
        
        // 判断是否是流级别的错误（只需要关闭流）
        bool isStreamError() const;
        
        operator bool() const { return m_type != kHttp2Error_Success; }
        
    private:
        Http2ErrorType m_type;
        std::string m_detail;
    };
}

#endif // GALAY_HTTP2_ERROR_H

