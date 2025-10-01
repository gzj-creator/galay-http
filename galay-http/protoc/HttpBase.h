#ifndef GALAY_HTTP_BASE_H
#define GALAY_HTTP_BASE_H

#include <string>
#include <unordered_map>
#include <galay/common/Base.h>
#include <galay/common/Log.h>

namespace galay::http 
{

    #define DEFAULT_HTTP_RECV_TIME_MS                       5 * 60 * 1000
    #define DEFAULT_HTTP_SEND_TIME_MS                       5 * 60 * 1000
    #define DEFAULT_HTTP_MAX_HEADER_SIZE                    8192                // http头最大长度8k
    #define DEFAULT_HTTP_MAX_BODY_SIZE                      1 * 1024 * 1024     // http body最大长度1M
    #define DEFAULT_HTTP_MAX_URI_LEN                        1024                // uri最大长度1k
    #define DEFAULT_HTTP_MAX_VERSION_SIZE                   32
    #define DEFAULT_HTTP_PEER_STEP_SIZE                     1024
    #define DEFAULT_HTTP_CHUNK_BUFFER_SIZE                  2048

    #define DEFAULT_HTTP_KEEPALIVE_TIME_MS                  (7500 * 1000)

    #define SERVER_NAME "galay-http"

    #define GALAY_SERVER SERVER_NAME "/" GALAY_VERSION

    inline std::atomic_int32_t gHttpMaxBodySize = DEFAULT_HTTP_MAX_BODY_SIZE;
    inline std::atomic_int32_t gHttpMaxUriSize = DEFAULT_HTTP_MAX_URI_LEN;
    inline std::atomic_int32_t gHttpMaxVersionSize = DEFAULT_HTTP_MAX_VERSION_SIZE;


    //log
    #define DEFAULT_LOG_METHOD_LENGTH       20
    #define DEFAULT_LOG_URI_PEER_LIMIT      50
    #define DEFAULT_LOG_STATUS_LENGTH       20
    #define DEFAULT_LOG_STATUS_TEXT_LENGTH  50

    enum class HttpMethod: int
    {
        Http_Method_Get = 0,
        Http_Method_Post = 1,
        Http_Method_Head = 2,
        Http_Method_Put = 3,
        Http_Method_Delete = 4,
        Http_Method_Trace = 5,
        Http_Method_Options = 6,
        Http_Method_Connect = 7,
        Http_Method_Patch = 8,
        Http_Method_Unknown,
    };

    enum class HttpVersion: int
    {
        Http_Version_1_0,   
        Http_Version_1_1,
        Http_Version_2_0,
        Http_Version_3_0,
        Http_Version_Unknown,
    };

    enum class HttpStatusCode: int
    {
        // Information responses
        Continue_100 = 100,
        SwitchingProtocol_101 = 101,
        Processing_102 = 102,
        EarlyHints_103 = 103,

        // Successful responses
        OK_200 = 200,
        Created_201 = 201,
        Accepted_202 = 202,
        NonAuthoritativeInformation_203 = 203,
        NoContent_204 = 204,
        ResetContent_205 = 205,
        PartialContent_206 = 206,
        MultiStatus_207 = 207,
        AlreadyReported_208 = 208,
        IMUsed_226 = 226,

        // Redirection messages
        MultipleChoices_300 = 300,
        MovedPermanently_301 = 301,
        Found_302 = 302,
        SeeOther_303 = 303,
        NotModified_304 = 304,
        UseProxy_305 = 305,
        Unused_306 = 306,
        TemporaryRedirect_307 = 307,
        PermanentRedirect_308 = 308,

        // Client error responses
        BadRequest_400 = 400,
        Unauthorized_401 = 401,
        PaymentRequired_402 = 402,
        Forbidden_403 = 403,
        NotFound_404 = 404,
        MethodNotAllowed_405 = 405,
        NotAcceptable_406 = 406,
        ProxyAuthenticationRequired_407 = 407,
        RequestTimeout_408 = 408,
        Conflict_409 = 409,
        Gone_410 = 410,
        LengthRequired_411 = 411,
        PreconditionFailed_412 = 412,
        PayloadTooLarge_413 = 413,
        UriTooLong_414 = 414,
        UnsupportedMediaType_415 = 415,
        RangeNotSatisfiable_416 = 416,
        ExpectationFailed_417 = 417,
        ImATeapot_418 = 418,
        MisdirectedRequest_421 = 421,
        UnprocessableContent_422 = 422,
        Locked_423 = 423,
        FailedDependency_424 = 424,
        TooEarly_425 = 425,
        UpgradeRequired_426 = 426,
        PreconditionRequired_428 = 428,
        TooManyRequests_429 = 429,
        RequestHeaderFieldsTooLarge_431 = 431,
        UnavailableForLegalReasons_451 = 451,

        // Server error responses
        InternalServerError_500 = 500,
        NotImplemented_501 = 501,
        BadGateway_502 = 502,
        ServiceUnavailable_503 = 503,
        GatewayTimeout_504 = 504,
        HttpVersionNotSupported_505 = 505,
        VariantAlsoNegotiates_506 = 506,
        InsufficientStorage_507 = 507,
        LoopDetected_508 = 508,
        NotExtended_510 = 510,
        NetworkAuthenticationRequired_511 = 511,
    };

    extern std::string httpVersionToString(HttpVersion version);
    extern HttpVersion stringToHttpVersion(std::string_view str);
    extern std::string httpMethodToString(HttpMethod method);
    extern HttpMethod stringToHttpMethod(std::string_view str);
    extern std::string httpStatusCodeToString(HttpStatusCode code); 

    class MimeType
    {
    public:
        static std::string convertToMimeType(const std::string& type);
    private:
        static std::unordered_map<std::string, std::string> mimeTypeMap;
    };
}

#endif
