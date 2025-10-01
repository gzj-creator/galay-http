#ifndef GALAY_HTTP_UTILS_H
#define GALAY_HTTP_UTILS_H 

#include "galay-http/protoc/HttpRequest.h"
#include "galay-http/protoc/HttpResponse.h"

namespace galay::http 
{ 
    class HttpUtils {
    public:
        static HttpRequest defaultGet(std::string_view uri);
        static HttpRequestHeader defaultGetHeader(std::string_view uri);

        //错误
        static HttpResponse defaultBadRequest();
        static HttpResponse defaultInternalServerError();
        static HttpResponse defaultNotFound();
        static HttpResponse defaultMethodNotAllowed();
        static HttpResponse defaultRequestTimeout();
        static HttpResponse defaultTooManyRequests();
        static HttpResponse defaultNotImplemented();
        static HttpResponse defaultServiceUnavailable();

        // 成功响应
        static HttpResponse defaultOk(const std::string& type, std::string&& body);
        static HttpResponseHeader defaultOkHeader(const std::string& type);
    };
}

#endif