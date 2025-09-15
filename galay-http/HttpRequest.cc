#include "HttpRequest.h"
namespace galay::http
{
    HttpRequestHeader &HttpRequest::header()
    {
       return m_header;
    }

    std::string HttpRequest::getBodyStr()
    {
        return std::move(m_body);
    }

    void HttpRequest::setHeader(HttpRequestHeader &&header)
    {
        m_header = std::move(header);
    }

    void HttpRequest::setHeader(HttpRequestHeader &header)
    {
        m_header.copyFrom(header);
    }

    void HttpRequest::setBodyStr(std::string &&body)
    {
        m_body = std::move(body);
    }

    std::string HttpRequest::toString() 
    {
        if(!m_header.isChunked()) {
            m_header.headerPairs().addHeaderPairIfNotExist("Content-Length", std::to_string(m_body.size()));
            return m_header.toString() + m_body;
        }
        return m_header.toString();
    }
}