#include "HttpUtils.h"

namespace galay::http 
{
    HttpRequest HttpUtils::defaultGet(std::string_view uri)
    {
        HttpRequest request;
        HttpRequestHeader header;
        header.method() = HttpMethod::Http_Method_Get;
        header.uri() = uri;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("User-Agent", SERVER_NAME);
        header.headerPairs().addHeaderPair("Accept", "*/*");
        request.setHeader(std::move(header));
        return request;
    }

    HttpRequest HttpUtils::defaultPost(std::string_view uri, std::string&& body)
    {
        HttpRequest request;
        HttpRequestHeader header;
        header.method() = HttpMethod::Http_Method_Post;
        header.uri() = uri;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("User-Agent", SERVER_NAME);
        header.headerPairs().addHeaderPair("Accept", "*/*");
        if (!body.empty()) {
            header.headerPairs().addHeaderPair("Content-Type", "application/json");
            header.headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));
        }
        request.setHeader(std::move(header));
        if (!body.empty()) {
            request.setBodyStr(std::move(body));
        }
        return request;
    }

    HttpRequest HttpUtils::defaultPut(std::string_view uri, std::string&& body)
    {
        HttpRequest request;
        HttpRequestHeader header;
        header.method() = HttpMethod::Http_Method_Put;
        header.uri() = uri;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("User-Agent", SERVER_NAME);
        header.headerPairs().addHeaderPair("Accept", "*/*");
        if (!body.empty()) {
            header.headerPairs().addHeaderPair("Content-Type", "application/json");
            header.headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));
        }
        request.setHeader(std::move(header));
        if (!body.empty()) {
            request.setBodyStr(std::move(body));
        }
        return request;
    }

    HttpRequest HttpUtils::defaultDelete(std::string_view uri)
    {
        HttpRequest request;
        HttpRequestHeader header;
        header.method() = HttpMethod::Http_Method_Delete;
        header.uri() = uri;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("User-Agent", SERVER_NAME);
        header.headerPairs().addHeaderPair("Accept", "*/*");
        request.setHeader(std::move(header));
        return request;
    }

    HttpRequest HttpUtils::defaultPatch(std::string_view uri, std::string&& body)
    {
        HttpRequest request;
        HttpRequestHeader header;
        header.method() = HttpMethod::Http_Method_Patch;
        header.uri() = uri;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("User-Agent", SERVER_NAME);
        header.headerPairs().addHeaderPair("Accept", "*/*");
        if (!body.empty()) {
            header.headerPairs().addHeaderPair("Content-Type", "application/json");
            header.headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));
        }
        request.setHeader(std::move(header));
        if (!body.empty()) {
            request.setBodyStr(std::move(body));
        }
        return request;
    }

    HttpRequest HttpUtils::defaultHead(std::string_view uri)
    {
        HttpRequest request;
        HttpRequestHeader header;
        header.method() = HttpMethod::Http_Method_Head;
        header.uri() = uri;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("User-Agent", SERVER_NAME);
        header.headerPairs().addHeaderPair("Accept", "*/*");
        request.setHeader(std::move(header));
        return request;
    }

    HttpRequest HttpUtils::defaultOptions(std::string_view uri)
    {
        HttpRequest request;
        HttpRequestHeader header;
        header.method() = HttpMethod::Http_Method_Options;
        header.uri() = uri;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("User-Agent", SERVER_NAME);
        header.headerPairs().addHeaderPair("Accept", "*/*");
        request.setHeader(std::move(header));
        return request;
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

    HttpResponse HttpUtils::defaultContinue()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::Continue_100;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>100 Continue</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultSwitchingProtocol()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::SwitchingProtocol_101;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>101 Switching Protocol</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultProcessing()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::Processing_102;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>102 Processing</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultEarlyHints()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::EarlyHints_103;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>103 Early Hints</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultCreated()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::Created_201;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>201 Created</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultAccepted()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::Accepted_202;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>202 Accepted</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultNonAuthoritativeInformation()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::NonAuthoritativeInformation_203;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>203 Non-Authoritative Information</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultNoContent()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::NoContent_204;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>204 No Content</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultResetContent()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::ResetContent_205;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>205 Reset Content</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultPartialContent()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::PartialContent_206;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>206 Partial Content</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultMultiStatus()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::MultiStatus_207;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>207 Multi-Status</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultAlreadyReported()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::AlreadyReported_208;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>208 Already Reported</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultIMUsed()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::IMUsed_226;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>226 IM Used</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultMultipleChoices()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::MultipleChoices_300;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>300 Multiple Choices</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultMovedPermanently()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::MovedPermanently_301;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>301 Moved Permanently</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultFound()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::Found_302;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>302 Found</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultSeeOther()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::SeeOther_303;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>303 See Other</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultNotModified()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::NotModified_304;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>304 Not Modified</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultUseProxy()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::UseProxy_305;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>305 Use Proxy</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultUnused()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::Unused_306;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>306 unused</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultTemporaryRedirect()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::TemporaryRedirect_307;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>307 Temporary Redirect</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultPermanentRedirect()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::PermanentRedirect_308;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>308 Permanent Redirect</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultUnauthorized()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::Unauthorized_401;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>401 Unauthorized</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultPaymentRequired()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::PaymentRequired_402;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>402 Payment Required</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultForbidden()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::Forbidden_403;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>403 Forbidden</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultConflict()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::Conflict_409;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>409 Conflict</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultGone()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::Gone_410;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>410 Gone</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultLengthRequired()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::LengthRequired_411;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>411 Length Required</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultPreconditionFailed()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::PreconditionFailed_412;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>412 Precondition Failed</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultPayloadTooLarge()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::PayloadTooLarge_413;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>413 Payload Too Large</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultUriTooLong()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::UriTooLong_414;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>414 URI Too Long</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultUnsupportedMediaType()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::UnsupportedMediaType_415;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>415 Unsupported Media Type</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultRangeNotSatisfiable()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::RangeNotSatisfiable_416;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>416 Range Not Satisfiable</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultExpectationFailed()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::ExpectationFailed_417;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>417 Expectation Failed</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultImATeapot()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::ImATeapot_418;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>418 I'm a teapot</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultMisdirectedRequest()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::MisdirectedRequest_421;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>421 Misdirected Request</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultUnprocessableContent()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::UnprocessableContent_422;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>422 Unprocessable Content</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultLocked()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::Locked_423;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>423 Locked</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultFailedDependency()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::FailedDependency_424;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>424 Failed Dependency</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultTooEarly()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::TooEarly_425;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>425 Too Early</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultUpgradeRequired()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::UpgradeRequired_426;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>426 Upgrade Required</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultPreconditionRequired()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::PreconditionRequired_428;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>428 Precondition Required</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultRequestHeaderFieldsTooLarge()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::RequestHeaderFieldsTooLarge_431;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>431 Request Header Fields Too Large</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultUnavailableForLegalReasons()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::UnavailableForLegalReasons_451;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>451 Unavailable For Legal Reasons</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultBadGateway()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::BadGateway_502;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>502 Bad Gateway</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultGatewayTimeout()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::GatewayTimeout_504;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>504 Gateway Timeout</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultHttpVersionNotSupported()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::HttpVersionNotSupported_505;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>505 HTTP Version Not Supported</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultVariantAlsoNegotiates()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::VariantAlsoNegotiates_506;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>506 Variant Also Negotiates</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultInsufficientStorage()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::InsufficientStorage_507;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>507 Insufficient Storage</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultLoopDetected()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::LoopDetected_508;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>508 Loop Detected</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultNotExtended()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::NotExtended_510;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>510 Not Extended</h1></body></html>");
        return response;
    }

    HttpResponse HttpUtils::defaultNetworkAuthenticationRequired()
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::NetworkAuthenticationRequired_511;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", "text/html");
        response.setHeader(std::move(header));
        response.setBodyStr("<html><body><h1>511 Network Authentication Required</h1></body></html>");
        return response;
    }

    // 成功响应
    HttpResponse HttpUtils::defaultOk(const std::string& type, std::string&& body)
    {
        HttpResponse response;
        HttpResponseHeader header;
        header.code() = HttpStatusCode::OK_200;
        header.version() = HttpVersion::Http_Version_1_1;
        header.headerPairs().addHeaderPair("Server", GALAY_SERVER);
        header.headerPairs().addHeaderPair("Content-Type", MimeType::convertToMimeType(type));
        response.setHeader(std::move(header));
        if(!body.empty()) response.setBodyStr(std::move(body));
        return response;
    }


    HttpResponse HttpUtils::defaultHttpResponse(HttpStatusCode code)
    {
        switch (code)
        {
            case HttpStatusCode::Continue_100:
                return defaultContinue();
            case HttpStatusCode::SwitchingProtocol_101:
                return defaultSwitchingProtocol();
            case HttpStatusCode::Processing_102:
                return defaultProcessing();
            case HttpStatusCode::EarlyHints_103:
                return defaultEarlyHints();
            case HttpStatusCode::OK_200:
                return defaultOk("html", "<html><body><h1>200 OK</h1></body></html>");
            case HttpStatusCode::Created_201:
                return defaultCreated();
            case HttpStatusCode::Accepted_202:
                return defaultAccepted();
            case HttpStatusCode::NonAuthoritativeInformation_203:
                return defaultNonAuthoritativeInformation();
            case HttpStatusCode::NoContent_204:
                return defaultNoContent();
            case HttpStatusCode::ResetContent_205:
                return defaultResetContent();
            case HttpStatusCode::PartialContent_206:
                return defaultPartialContent();
            case HttpStatusCode::MultiStatus_207:
                return defaultMultiStatus();
            case HttpStatusCode::AlreadyReported_208:
                return defaultAlreadyReported();
            case HttpStatusCode::IMUsed_226:
                return defaultIMUsed();
            case HttpStatusCode::MultipleChoices_300:
                return defaultMultipleChoices();
            case HttpStatusCode::MovedPermanently_301:
                return defaultMovedPermanently();
            case HttpStatusCode::Found_302:
                return defaultFound();
            case HttpStatusCode::SeeOther_303:
                return defaultSeeOther();
            case HttpStatusCode::NotModified_304:
                return defaultNotModified();
            case HttpStatusCode::UseProxy_305:
                return defaultUseProxy();
            case HttpStatusCode::Unused_306:
                return defaultUnused();
            case HttpStatusCode::TemporaryRedirect_307:
                return defaultTemporaryRedirect();
            case HttpStatusCode::PermanentRedirect_308:
                return defaultPermanentRedirect();
            case HttpStatusCode::BadRequest_400:
                return defaultBadRequest();
            case HttpStatusCode::Unauthorized_401:
                return defaultUnauthorized();
            case HttpStatusCode::PaymentRequired_402:
                return defaultPaymentRequired();
            case HttpStatusCode::Forbidden_403:
                return defaultForbidden();
            case HttpStatusCode::NotFound_404:
                return defaultNotFound();
            case HttpStatusCode::MethodNotAllowed_405:
                return defaultMethodNotAllowed();
            case HttpStatusCode::NotAcceptable_406:
                return defaultNotFound(); // Reuse NotFound for simplicity
            case HttpStatusCode::ProxyAuthenticationRequired_407:
                return defaultRequestTimeout(); // Reuse RequestTimeout for simplicity
            case HttpStatusCode::RequestTimeout_408:
                return defaultRequestTimeout();
            case HttpStatusCode::Conflict_409:
                return defaultConflict();
            case HttpStatusCode::Gone_410:
                return defaultGone();
            case HttpStatusCode::LengthRequired_411:
                return defaultLengthRequired();
            case HttpStatusCode::PreconditionFailed_412:
                return defaultPreconditionFailed();
            case HttpStatusCode::PayloadTooLarge_413:
                return defaultPayloadTooLarge();
            case HttpStatusCode::UriTooLong_414:
                return defaultUriTooLong();
            case HttpStatusCode::UnsupportedMediaType_415:
                return defaultUnsupportedMediaType();
            case HttpStatusCode::RangeNotSatisfiable_416:
                return defaultRangeNotSatisfiable();
            case HttpStatusCode::ExpectationFailed_417:
                return defaultExpectationFailed();
            case HttpStatusCode::ImATeapot_418:
                return defaultImATeapot();
            case HttpStatusCode::MisdirectedRequest_421:
                return defaultMisdirectedRequest();
            case HttpStatusCode::UnprocessableContent_422:
                return defaultUnprocessableContent();
            case HttpStatusCode::Locked_423:
                return defaultLocked();
            case HttpStatusCode::FailedDependency_424:
                return defaultFailedDependency();
            case HttpStatusCode::TooEarly_425:
                return defaultTooEarly();
            case HttpStatusCode::UpgradeRequired_426:
                return defaultUpgradeRequired();
            case HttpStatusCode::PreconditionRequired_428:
                return defaultPreconditionRequired();
            case HttpStatusCode::TooManyRequests_429:
                return defaultTooManyRequests();
            case HttpStatusCode::RequestHeaderFieldsTooLarge_431:
                return defaultRequestHeaderFieldsTooLarge();
            case HttpStatusCode::UnavailableForLegalReasons_451:
                return defaultUnavailableForLegalReasons();
            case HttpStatusCode::InternalServerError_500:
                return defaultInternalServerError();
            case HttpStatusCode::NotImplemented_501:
                return defaultNotImplemented();
            case HttpStatusCode::BadGateway_502:
                return defaultBadGateway();
            case HttpStatusCode::ServiceUnavailable_503:
                return defaultServiceUnavailable();
            case HttpStatusCode::GatewayTimeout_504:
                return defaultGatewayTimeout();
            case HttpStatusCode::HttpVersionNotSupported_505:
                return defaultHttpVersionNotSupported();
            case HttpStatusCode::VariantAlsoNegotiates_506:
                return defaultVariantAlsoNegotiates();
            case HttpStatusCode::InsufficientStorage_507:
                return defaultInsufficientStorage();
            case HttpStatusCode::LoopDetected_508:
                return defaultLoopDetected();
            case HttpStatusCode::NotExtended_510:
                return defaultNotExtended();
            case HttpStatusCode::NetworkAuthenticationRequired_511:
                return defaultNetworkAuthenticationRequired();
            default:
                return defaultInternalServerError();
        }
    }
}