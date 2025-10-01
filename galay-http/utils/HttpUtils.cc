#include "HttpUtils.h"

namespace galay::http 
{
    HttpRequest HttpUtils::defaultGet(std::string_view uri)
    {
        HttpRequest request;
        request.setHeader(defaultGetHeader(uri));
        return request;
    }

    HttpRequestHeader HttpUtils::defaultGetHeader(std::string_view uri)
    {
        HttpRequestHeader header;
        header.method() = HttpMethod::Http_Method_Get;
        header.uri() = uri;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("User-Agent", SERVER_NAME);
        header.headerPairs().addHeaderPair("Accept", "*/*");
        return header;
    }

    HttpResponse HttpUtils::defaultBadRequest()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::BadRequest_400;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>400 Bad Request</h1></body></html>");
        return response;
    }

    
    HttpResponse HttpUtils::defaultInternalServerError()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::InternalServerError_500;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>500 Internal Server Error</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultNotFound()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::NotFound_404;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>404 Not Found</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultMethodNotAllowed()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::MethodNotAllowed_405;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>405 Method Not Allowed</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultRequestTimeout()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::RequestTimeout_408;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>408 Request Timeout</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultTooManyRequests()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::TooManyRequests_429;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>429 Too Many Requests</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultNotImplemented()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::NotImplemented_501;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>501 Not Implemented</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultServiceUnavailable()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::ServiceUnavailable_503;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>503 Service Unavailable</h1></body></html>");
        return response;
    }

    // 成功响应
    HttpResponse HttpUtils::defaultOk(const std::string& type, std::string&& body)
    {
        HttpResponse response;
        response.setHeader(defaultOkHeader(type));
        response.setBodyStr(std::move(body));
        return response;
    }

    HttpResponseHeader HttpUtils::defaultOkHeader(const std::string &type)
    {
        HttpResponseHeader header;
        header.code() = HttpStatusCode::OK_200;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", MimeType::convertToMimeType(type));
        return header;
    }
}