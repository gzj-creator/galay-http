#ifndef GALAY_HTTP_UTILS_H
#define GALAY_HTTP_UTILS_H 

#include "galay-http/protoc/HttpRequest.h"
#include "galay-http/protoc/HttpResponse.h"

namespace galay::http 
{ 
    class HttpUtils {
    public:
        // HTTP 请求方法
        static HttpRequest defaultGet(std::string_view uri);
        static HttpRequest defaultPost(std::string_view uri, std::string&& body = "");
        static HttpRequest defaultPut(std::string_view uri, std::string&& body = "");
        static HttpRequest defaultDelete(std::string_view uri);
        static HttpRequest defaultPatch(std::string_view uri, std::string&& body = "");
        static HttpRequest defaultHead(std::string_view uri);
        static HttpRequest defaultOptions(std::string_view uri);

        //错误
        static HttpResponse defaultBadRequest();
        static HttpResponse defaultInternalServerError();
        static HttpResponse defaultNotFound();
        static HttpResponse defaultMethodNotAllowed();
        static HttpResponse defaultRequestTimeout();
        static HttpResponse defaultTooManyRequests();
        static HttpResponse defaultNotImplemented();
        static HttpResponse defaultServiceUnavailable();
        static HttpResponse defaultContinue();
        static HttpResponse defaultSwitchingProtocol();
        static HttpResponse defaultProcessing();
        static HttpResponse defaultEarlyHints();
        static HttpResponse defaultCreated();
        static HttpResponse defaultAccepted();
        static HttpResponse defaultNonAuthoritativeInformation();
        static HttpResponse defaultNoContent();
        static HttpResponse defaultResetContent();
        static HttpResponse defaultPartialContent();
        static HttpResponse defaultMultiStatus();
        static HttpResponse defaultAlreadyReported();
        static HttpResponse defaultIMUsed();
        static HttpResponse defaultMultipleChoices();
        static HttpResponse defaultMovedPermanently();
        static HttpResponse defaultFound();
        static HttpResponse defaultSeeOther();
        static HttpResponse defaultNotModified();
        static HttpResponse defaultUseProxy();
        static HttpResponse defaultUnused();
        static HttpResponse defaultTemporaryRedirect();
        static HttpResponse defaultPermanentRedirect();
        static HttpResponse defaultUnauthorized();
        static HttpResponse defaultPaymentRequired();
        static HttpResponse defaultForbidden();
        static HttpResponse defaultConflict();
        static HttpResponse defaultGone();
        static HttpResponse defaultLengthRequired();
        static HttpResponse defaultPreconditionFailed();
        static HttpResponse defaultPayloadTooLarge();
        static HttpResponse defaultUriTooLong();
        static HttpResponse defaultUnsupportedMediaType();
        static HttpResponse defaultRangeNotSatisfiable();
        static HttpResponse defaultExpectationFailed();
        static HttpResponse defaultImATeapot();
        static HttpResponse defaultMisdirectedRequest();
        static HttpResponse defaultUnprocessableContent();
        static HttpResponse defaultLocked();
        static HttpResponse defaultFailedDependency();
        static HttpResponse defaultTooEarly();
        static HttpResponse defaultUpgradeRequired();
        static HttpResponse defaultPreconditionRequired();
        static HttpResponse defaultRequestHeaderFieldsTooLarge();
        static HttpResponse defaultUnavailableForLegalReasons();
        static HttpResponse defaultBadGateway();
        static HttpResponse defaultGatewayTimeout();
        static HttpResponse defaultHttpVersionNotSupported();
        static HttpResponse defaultVariantAlsoNegotiates();
        static HttpResponse defaultInsufficientStorage();
        static HttpResponse defaultLoopDetected();
        static HttpResponse defaultNotExtended();
        static HttpResponse defaultNetworkAuthenticationRequired();

        static HttpResponse defaultOk(const std::string& type, std::string&& body);

        static HttpResponse defaultHttpResponse(HttpStatusCode code);
    };
}

#endif