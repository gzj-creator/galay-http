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
        "Not found",                            // kHttpError_NotFound  
        "Unknown error",                      // kHttpError_UnknownError
    };


    HttpError::HttpError(HttpErrorCode code)
        :m_code(code)
    {
    }

    HttpErrorCode HttpError::code() const
    {
        return m_code;
    }

    std::string HttpError::message() const
    {
        if(static_cast<uint32_t>(m_code) >= sizeof(g_http_error_messages)) {
            return "Unknown Http error";
        }
        return g_http_error_messages[m_code];
    }

    HttpStatusCode HttpError::toHttpStatusCode() const
    {
        switch (m_code)
        {
            case kHttpError_NoError:
                return HttpStatusCode::OK_200;
                
            case kHttpError_ConnectionClose:
                return HttpStatusCode::InternalServerError_500;
                
            case kHttpError_TcpRecvError:
            case kHttpError_TcpSendError:
                return HttpStatusCode::InternalServerError_500;
                
            case kHttpError_RequestTimeOut:
            case kHttpError_RecvTimeOut:
            case kHttpError_SendTimeOut:
                return HttpStatusCode::RequestTimeout_408;
                
            case kHttpError_ContentLengthNotContained:
                return HttpStatusCode::LengthRequired_411;
                
            case kHttpError_ContentLengthConvertError:
            case kHttpError_BodyLengthNotMatch:
                return HttpStatusCode::BadRequest_400;
                
            case kHttpError_HeaderInComplete:
            case kHttpError_BadRequest:
            case kHttpError_UrlInvalid:
            case kHttpError_UriEncodeError:
            case kHttpError_InvalidChunkFormat:
            case kHttpError_InvalidChunkLength:
            case kHttpError_ChunkHasError:
                return HttpStatusCode::BadRequest_400;
                
            case kHttpError_BodyInComplete:
                return HttpStatusCode::BadRequest_400;
                
            case kHttpError_HeaderTooLong:
                return HttpStatusCode::RequestHeaderFieldsTooLarge_431;
                
            case kHttpError_UriTooLong:
                return HttpStatusCode::UriTooLong_414;
                
            case kHttpError_HttpCodeInvalid:
                return HttpStatusCode::BadRequest_400;
                
            case kHttpError_HeaderPairExist:
            case kHttpError_HeaderPairNotExist:
                return HttpStatusCode::BadRequest_400;
                
            case kHttpError_PortInvalid:
                return HttpStatusCode::BadRequest_400;
                
            case kHttpError_MethodNotAllow:
                return HttpStatusCode::MethodNotAllowed_405;
                
            case kHttpError_VersionNotSupport:
                return HttpStatusCode::HttpVersionNotSupported_505;
                
            case kHttpError_RequestEntityTooLarge:
                return HttpStatusCode::PayloadTooLarge_413;
                
            case kHttpError_ContentTypeInvalid:
                return HttpStatusCode::UnsupportedMediaType_415;
            case kHttpError_NotFound:
                return HttpStatusCode::NotFound_404;
            case kHttpError_UnknownError:
                return HttpStatusCode::InternalServerError_500;
            
            default:
                return HttpStatusCode::InternalServerError_500;
        }
    }
}