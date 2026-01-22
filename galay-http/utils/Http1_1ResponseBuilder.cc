/**
 * @file Http1_1ResponseBuilder.cc
 * @brief HTTP/1.1 响应构造器实现
 */

#include "Http1_1ResponseBuilder.h"

namespace galay::http
{

Http1_1ResponseBuilder::Http1_1ResponseBuilder()
{
    // 设置默认值
    m_response.header().version() = HttpVersion::HttpVersion_1_1;
    m_response.header().code() = HttpStatusCode::OK_200;
}

Http1_1ResponseBuilder& Http1_1ResponseBuilder::status(int code)
{
    m_response.header().code() = static_cast<HttpStatusCode>(code);
    return *this;
}

Http1_1ResponseBuilder& Http1_1ResponseBuilder::status(HttpStatusCode code)
{
    m_response.header().code() = code;
    return *this;
}

Http1_1ResponseBuilder& Http1_1ResponseBuilder::header(const std::string& key, const std::string& value)
{
    m_response.header().headerPairs().addHeaderPair(key, value);
    return *this;
}

Http1_1ResponseBuilder& Http1_1ResponseBuilder::headers(const std::map<std::string, std::string>& headers)
{
    for (const auto& [key, value] : headers) {
        m_response.header().headerPairs().addHeaderPair(key, value);
    }
    return *this;
}

Http1_1ResponseBuilder& Http1_1ResponseBuilder::contentType(const std::string& contentType)
{
    return header("Content-Type", contentType);
}

Http1_1ResponseBuilder& Http1_1ResponseBuilder::body(const std::string& body)
{
    m_body = body;
    return *this;
}

Http1_1ResponseBuilder& Http1_1ResponseBuilder::body(std::string&& body)
{
    m_body = std::move(body);
    return *this;
}

Http1_1ResponseBuilder& Http1_1ResponseBuilder::json(const std::string& json)
{
    contentType("application/json; charset=utf-8");
    m_body = json;
    return *this;
}

Http1_1ResponseBuilder& Http1_1ResponseBuilder::html(const std::string& html)
{
    contentType("text/html; charset=utf-8");
    m_body = html;
    return *this;
}

Http1_1ResponseBuilder& Http1_1ResponseBuilder::text(const std::string& text)
{
    contentType("text/plain; charset=utf-8");
    m_body = text;
    return *this;
}

HttpResponse Http1_1ResponseBuilder::build()
{
    HttpResponse response_copy;
    response_copy.header() = m_response.header();

    if (!m_body.empty()) {
        std::string body_copy = m_body;
        response_copy.setBodyStr(std::move(body_copy));
    }
    return response_copy;
}

HttpResponse Http1_1ResponseBuilder::buildMove()
{
    if (!m_body.empty()) {
        m_response.setBodyStr(std::move(m_body));
    }
    return std::move(m_response);
}

// 静态工厂方法

Http1_1ResponseBuilder Http1_1ResponseBuilder::ok()
{
    Http1_1ResponseBuilder builder;
    builder.status(HttpStatusCode::OK_200);
    return builder;
}

Http1_1ResponseBuilder Http1_1ResponseBuilder::created()
{
    Http1_1ResponseBuilder builder;
    builder.status(HttpStatusCode::Created_201);
    return builder;
}

Http1_1ResponseBuilder Http1_1ResponseBuilder::noContent()
{
    Http1_1ResponseBuilder builder;
    builder.status(HttpStatusCode::NoContent_204);
    return builder;
}

Http1_1ResponseBuilder Http1_1ResponseBuilder::badRequest()
{
    Http1_1ResponseBuilder builder;
    builder.status(HttpStatusCode::BadRequest_400);
    return builder;
}

Http1_1ResponseBuilder Http1_1ResponseBuilder::unauthorized()
{
    Http1_1ResponseBuilder builder;
    builder.status(HttpStatusCode::Unauthorized_401);
    return builder;
}

Http1_1ResponseBuilder Http1_1ResponseBuilder::forbidden()
{
    Http1_1ResponseBuilder builder;
    builder.status(HttpStatusCode::Forbidden_403);
    return builder;
}

Http1_1ResponseBuilder Http1_1ResponseBuilder::notFound()
{
    Http1_1ResponseBuilder builder;
    builder.status(HttpStatusCode::NotFound_404);
    return builder;
}

Http1_1ResponseBuilder Http1_1ResponseBuilder::internalServerError()
{
    Http1_1ResponseBuilder builder;
    builder.status(HttpStatusCode::InternalServerError_500);
    return builder;
}

} // namespace galay::http
