#include "HttpError.h"

namespace galay::http
{
    const char* g_http_error_messages[] = {
        "No error",                           // kNoError
        "Connection closed",                  // kConnectionClose
        "Tcp recv error",                     // kTcpRecvError"
        "Tcp send error",                     // kTcpSendError"
        "Request timeout",                    // kRequestTimeOut
        "Not contains Content-Length",        // kContentLengthNotContained"
        "Content-Length convert error",       // kContentLengthConvertError
        "HTTP header incomplete",             // kHeaderInComplete
        "HTTP body incomplete",               // kBodyInComplete
        "HTTP header too long",               // kHeaderTooLong
        "URI too long",                       // kUriTooLong
        "Chunked transfer encoding error",    // kChunkHasError
        "Invalid HTTP status code",           // kHttpCodeInvalid
        "HTTP header key-value pair already exists",  // kHeaderPairExist
        "HTTP header key-value pair does not exist",  // kHeaderPairNotExist
        "Bad request format",                 // kBadRequest
        "Invalid URL format",                 // kUrlInvalid
        "Invalid port number",                // kPortInvalid
        "HTTP method not allowed",            // kMethodNotAllow
        "HTTP version not supported",         // kVersionNotSupport
        "Request entity too large",           // kRequestEntityTooLarge
        "URI encoding error",                 // kUriEncodeError
        "Invalid Content-Type",               // kContentTypeInvalid
        "Invalid chunk format",               // kInvalidChunkFormat
        "Invalid chunk length",
        "Body length not match Content-Length",// kBodyLengthNotMatch
        "Recv time out",                       // kRecvTimeOut
        "Send timeout",                         //kSendTimeOut
        "Not found",                            // kNotFound  
        "Not implemented",                      // kNotImplemented
        "Upgrade failed",                       // kUpgradeFailed
        "Unknown error",                      // kUnknownError
    };


    HttpError::HttpError(HttpErrorCode code, const std::string& extra_msg)
        :m_code(code), m_extra_msg(extra_msg)
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
            case kNoError:
                return HttpStatusCode::OK_200;
                
            case kConnectionClose:
                return HttpStatusCode::InternalServerError_500;
                
            case kTcpRecvError:
            case kTcpSendError:
                return HttpStatusCode::InternalServerError_500;
                
            case kRequestTimeOut:
            case kRecvTimeOut:
            case kSendTimeOut:
                return HttpStatusCode::RequestTimeout_408;
                
            case kContentLengthNotContained:
                return HttpStatusCode::LengthRequired_411;
                
            case kContentLengthConvertError:
            case kBodyLengthNotMatch:
                return HttpStatusCode::BadRequest_400;
                
            case kHeaderInComplete:
            case kBadRequest:
            case kUrlInvalid:
            case kUriEncodeError:
            case kInvalidChunkFormat:
            case kInvalidChunkLength:
            case kChunkHasError:
                return HttpStatusCode::BadRequest_400;
                
            case kBodyInComplete:
                return HttpStatusCode::BadRequest_400;
                
            case kHeaderTooLong:
                return HttpStatusCode::RequestHeaderFieldsTooLarge_431;
                
            case kUriTooLong:
                return HttpStatusCode::UriTooLong_414;
                
            case kHttpCodeInvalid:
                return HttpStatusCode::BadRequest_400;
                
            case kHeaderPairExist:
            case kHeaderPairNotExist:
                return HttpStatusCode::BadRequest_400;
                
            case kPortInvalid:
                return HttpStatusCode::BadRequest_400;
                
            case kMethodNotAllow:
                return HttpStatusCode::MethodNotAllowed_405;
                
            case kVersionNotSupport:
                return HttpStatusCode::HttpVersionNotSupported_505;
                
            case kRequestEntityTooLarge:
                return HttpStatusCode::PayloadTooLarge_413;
                
            case kContentTypeInvalid:
                return HttpStatusCode::UnsupportedMediaType_415;
            case kNotFound:
                return HttpStatusCode::NotFound_404;
            case kUnknownError:
                return HttpStatusCode::InternalServerError_500;
            case kNotImplemented:
                return HttpStatusCode::NotImplemented_501;
            case kUpgradeFailed:
                return HttpStatusCode::UpgradeRequired_426;
            default:
                return HttpStatusCode::InternalServerError_500;
        }
    }
}