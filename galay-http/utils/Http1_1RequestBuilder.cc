/**
 * @file Http1_1RequestBuilder.cc
 * @brief HTTP/1.1 请求构造器实现
 */

#include "Http1_1RequestBuilder.h"
#include <sstream>

namespace galay::http
{

Http1_1RequestBuilder::Http1_1RequestBuilder()
{
    // 设置默认值
    m_request.header().version() = HttpVersion::HttpVersion_1_1;
    m_request.header().method() = HttpMethod::GET;
}

Http1_1RequestBuilder& Http1_1RequestBuilder::method(HttpMethod method)
{
    m_request.header().method() = method;
    return *this;
}

Http1_1RequestBuilder& Http1_1RequestBuilder::uri(const std::string& uri)
{
    m_request.header().uri() = uri;
    return *this;
}

Http1_1RequestBuilder& Http1_1RequestBuilder::header(const std::string& key, const std::string& value)
{
    m_request.header().headerPairs().addHeaderPair(key, value);
    return *this;
}

Http1_1RequestBuilder& Http1_1RequestBuilder::headers(const std::map<std::string, std::string>& headers)
{
    for (const auto& [key, value] : headers) {
        m_request.header().headerPairs().addHeaderPair(key, value);
    }
    return *this;
}

Http1_1RequestBuilder& Http1_1RequestBuilder::host(const std::string& host)
{
    return header("Host", host);
}

Http1_1RequestBuilder& Http1_1RequestBuilder::contentType(const std::string& contentType)
{
    return header("Content-Type", contentType);
}

Http1_1RequestBuilder& Http1_1RequestBuilder::userAgent(const std::string& userAgent)
{
    return header("User-Agent", userAgent);
}

Http1_1RequestBuilder& Http1_1RequestBuilder::connection(const std::string& connection)
{
    return header("Connection", connection);
}

Http1_1RequestBuilder& Http1_1RequestBuilder::body(const std::string& body)
{
    m_body = body;
    return *this;
}

Http1_1RequestBuilder& Http1_1RequestBuilder::body(std::string&& body)
{
    m_body = std::move(body);
    return *this;
}

Http1_1RequestBuilder& Http1_1RequestBuilder::json(const std::string& json)
{
    contentType("application/json; charset=utf-8");
    m_body = json;
    return *this;
}

Http1_1RequestBuilder& Http1_1RequestBuilder::form(const std::map<std::string, std::string>& form)
{
    contentType("application/x-www-form-urlencoded");

    std::ostringstream oss;
    bool first = true;
    for (const auto& [key, value] : form) {
        if (!first) {
            oss << "&";
        }
        oss << key << "=" << value;  // TODO: URL encode
        first = false;
    }

    m_body = oss.str();
    return *this;
}

HttpRequest Http1_1RequestBuilder::build()
{
    HttpRequest request_copy;
    request_copy.header() = m_request.header();

    if (!m_body.empty()) {
        std::string body_copy = m_body;
        request_copy.setBodyStr(std::move(body_copy));
    }
    return request_copy;
}

HttpRequest Http1_1RequestBuilder::buildMove()
{
    if (!m_body.empty()) {
        m_request.setBodyStr(std::move(m_body));
    }
    return std::move(m_request);
}

// 静态工厂方法

Http1_1RequestBuilder Http1_1RequestBuilder::get(const std::string& uri)
{
    Http1_1RequestBuilder builder;
    builder.method(HttpMethod::GET).uri(uri);
    return builder;
}

Http1_1RequestBuilder Http1_1RequestBuilder::post(const std::string& uri)
{
    Http1_1RequestBuilder builder;
    builder.method(HttpMethod::POST).uri(uri);
    return builder;
}

Http1_1RequestBuilder Http1_1RequestBuilder::put(const std::string& uri)
{
    Http1_1RequestBuilder builder;
    builder.method(HttpMethod::PUT).uri(uri);
    return builder;
}

Http1_1RequestBuilder Http1_1RequestBuilder::del(const std::string& uri)
{
    Http1_1RequestBuilder builder;
    builder.method(HttpMethod::DELETE).uri(uri);
    return builder;
}

Http1_1RequestBuilder Http1_1RequestBuilder::patch(const std::string& uri)
{
    Http1_1RequestBuilder builder;
    builder.method(HttpMethod::PATCH).uri(uri);
    return builder;
}

Http1_1RequestBuilder Http1_1RequestBuilder::head(const std::string& uri)
{
    Http1_1RequestBuilder builder;
    builder.method(HttpMethod::HEAD).uri(uri);
    return builder;
}

Http1_1RequestBuilder Http1_1RequestBuilder::options(const std::string& uri)
{
    Http1_1RequestBuilder builder;
    builder.method(HttpMethod::OPTIONS).uri(uri);
    return builder;
}

} // namespace galay::http
