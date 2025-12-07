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
        }
        
        std::string header_str = m_header.toString();
        
        if(m_header.isChunked()) {
            return header_str;
        }
        
        // 预分配结果字符串，避免临时字符串
        std::string result;
        result.reserve(header_str.size() + m_body.size());
        result += header_str;
        result += m_body;
        return result;
    }
}