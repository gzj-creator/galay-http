#ifndef GALAY_HTTP_ERROR_H
#define GALAY_HTTP_ERROR_H 

#include <galay/common/Error.h>
#include "HttpBase.h"

namespace galay::http
{

    enum HttpErrorCode
    {
        kHttpError_NoError = 0,              // 无错误
        kHttpError_ConnectionClose,          // 连接已关闭
        kHttpError_TcpRecvError,             // TCP接收错误
        kHttpError_TcpSendError,
        kHttpError_RequestTimeOut,           // 请求超时
        kHttpError_ContentLengthNotContained,// 缺少Content-Length
        kHttpError_ContentLengthConvertError,// Content-Length转换错误
        kHttpError_HeaderInComplete,         // HTTP头部不完整
        kHttpError_BodyInComplete,           // HTTP体不完整
        kHttpError_HeaderTooLong,            // HTTP头部过长
        kHttpError_UriTooLong,               // URI过长
        kHttpError_ChunkHasError,            // 分块传输编码错误
        kHttpError_HttpCodeInvalid,          // HTTP状态码无效
        kHttpError_HeaderPairExist,          // HTTP头部键值对已存在
        kHttpError_HeaderPairNotExist,       // HTTP头部键值对不存在
        kHttpError_BadRequest,               // 错误的请求格式
        kHttpError_UrlInvalid,               // URL格式无效
        kHttpError_PortInvalid,              // 端口号无效
        kHttpError_MethodNotAllow,           // HTTP方法不支持
        kHttpError_VersionNotSupport,        // HTTP版本不支持
        kHttpError_RequestEntityTooLarge,    // 请求体过大
        kHttpError_UriEncodeError,           // URI编码错误
        kHttpError_ContentTypeInvalid,       // Content-Type无效
        kHttpError_InvalidChunkFormat,       // Chunk格式错误
        kHttpError_InvalidChunkLength,       // Chunk长度错误
        kHttpError_BodyLengthNotMatch,       // 请求体长度与Content-Length不匹配
        kHttpError_RecvTimeOut,              // 接收超时
        kHttpError_SendTimeOut,              // 发送超时
        kHttpError_NotFound,                 // 未找到
        kHttpError_NotImplemented,           // 未实现
        kHttpError_UpgradeFailed,            // 升级失败
        kHttpError_UnknownError,              // 未知错误
    };

    class HttpError
    {
    public:
        HttpError(HttpErrorCode code);
        HttpErrorCode code() const;
        std::string message() const;
        HttpStatusCode toHttpStatusCode() const;
    private:
        HttpErrorCode m_code;
    };
    

}


#endif