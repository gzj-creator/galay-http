#ifndef GALAY_HTTP_ERROR_H
#define GALAY_HTTP_ERROR_H

#include "HttpBase.h"
#include <string>

namespace galay::http
{

    enum HttpErrorCode
    {
        kNoError = 0,              // 无错误
        kIncomplete,               // 数据不完整，需要更多数据
        kConnectionClose,          // 连接已关闭
        kTcpRecvError,             // TCP接收错误
        kTcpSendError,             // TCP发送失败
        kRequestTimeOut,           // 请求超时
        kContentLengthNotContained,// 缺少Content-Length
        kContentLengthConvertError,// Content-Length转换错误
        kHeaderInComplete,         // HTTP头部不完整
        kBodyInComplete,           // HTTP体不完整
        kHeaderTooLong,            // HTTP头部过长
        kUriTooLong,               // URI过长
        kChunkHasError,            // 分块传输编码错误
        kHttpCodeInvalid,          // HTTP状态码无效
        kHeaderPairExist,          // HTTP头部键值对已存在
        kHeaderPairNotExist,       // HTTP头部键值对不存在
        kBadRequest,               // 错误的请求格式
        kUrlInvalid,               // URL格式无效
        kPortInvalid,              // 端口号无效
        kMethodNotAllow,           // HTTP方法不支持
        kVersionNotSupport,        // HTTP版本不支持
        kRequestEntityTooLarge,    // 请求体过大
        kUriEncodeError,           // URI编码错误
        kContentTypeInvalid,       // Content-Type无效
        kInvalidChunkFormat,       // Chunk格式错误
        kInvalidChunkLength,       // Chunk长度错误
        kBodyLengthNotMatch,       // 请求体长度与Content-Length不匹配
        kRecvTimeOut,              // 接收超时
        kSendTimeOut,              // 发送超时
        kNotFound,                 // 未找到
        kNotImplemented,           // 未实现
        kUpgradeFailed,            // 升级失败
        kUnknownError,              // 未知错误
        kHeaderTooLarge,            // HTTP头部过大
        kRecvError,                 // 接收错误
        kSendError,                 // 发送错误
        kCloseError,                // 关闭错误
        kInternalError,             // 内部错误
        kTcpConnectError,           // TCP连接错误
        kChunkSizeConvertError,     // Chunk大小转换错误
    };

    class HttpError
    {
    public:
        HttpError(HttpErrorCode code, const std::string& extra_msg = "");
        HttpErrorCode code() const;
        std::string message() const;
        HttpStatusCode toHttpStatusCode() const;
    private:
        HttpErrorCode m_code;
        std::string m_extra_msg;
    };
    

}


#endif