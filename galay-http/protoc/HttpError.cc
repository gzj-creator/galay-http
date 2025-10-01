#include "HttpError.h"

namespace galay::http
{
    const char* g_http_error_messages[] = {
        "No error",                           // kHttpError_NoError
        "Connection closed",                  // kHttpError_ConnectionClose
        "Tcp recv error",                     // kHttpError_TcpRecvError"
        "Tcp send error",                     // kHttpError_TcpSendError"
        "Request timeout",                    // kHttpError_RequestTimeOut
        "Not contains Content-Length",        // kHttpError_ContentLengthNotContained"
        "Content-Length convert error",       // kHttpError_ContentLengthConvertError
        "HTTP header incomplete",             // kHttpError_HeaderInComplete
        "HTTP body incomplete",               // kHttpError_BodyInComplete
        "HTTP header too long",               // kHttpError_HeaderTooLong
        "URI too long",                       // kHttpError_UriTooLong
        "Chunked transfer encoding error",    // kHttpError_ChunkHasError
        "Invalid HTTP status code",           // kHttpError_HttpCodeInvalid
        "HTTP header key-value pair already exists",  // kHttpError_HeaderPairExist
        "HTTP header key-value pair does not exist",  // kHttpError_HeaderPairNotExist
        "Bad request format",                 // kHttpError_BadRequest
        "Invalid URL format",                 // kHttpError_UrlInvalid
        "Invalid port number",                // kHttpError_PortInvalid
        "Unknown error",                      // kHttpError_UnkownError
        "HTTP method not allowed",            // kHttpError_MethodNotAllow
        "HTTP version not supported",         // kHttpError_VersionNotSupport
        "Request entity too large",           // kHttpError_RequestEntityTooLarge
        "URI encoding error",                 // kHttpError_UriEncodeError
        "Invalid Content-Type",               // kHttpError_ContentTypeInvalid
        "Invalid chunk format",               // kHttpError_InvalidChunkFormat
        "Invalid chunk length",
        "Body length not match Content-Length",// kHttpError_BodyLengthNotMatch
        "Recv time out",                       // kHttpError_RecvTimeOut
        "Send timeout",                         //kHttpError_SendTimeOut
        "Unknown error",                      // kHttpError_UnknownError
    };


    HttpError::HttpError(HttpErrorCode code)
        :m_code(code)
    {
    }

    std::string HttpError::message() const
    {
        if(static_cast<uint32_t>(m_code) >= sizeof(g_http_error_messages)) {
            return "Unknown Http error";
        }
        return g_http_error_messages[m_code];
    }
}