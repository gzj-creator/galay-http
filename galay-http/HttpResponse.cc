#include "HttpResponse.h"

namespace galay::http
{ 
    HttpResponseHeader &HttpResponse::header()
    {
       return m_header;
    }

    std::string HttpResponse::getBodyStr()
    {
        return std::move(m_body);
    }

    void HttpResponse::setHeader(HttpResponseHeader &&header)
    {
        m_header = std::move(header);
    }

    void HttpResponse::setHeader(HttpResponseHeader &header)
    {
        m_header.copyFrom(header);
    }

    void HttpResponse::setBodyStr(std::string &&body)
    {
        m_body = std::move(body);
    }

    std::string HttpResponse::toString() 
    {
        if(!m_header.isChunked()) {
            m_header.headerPairs().addHeaderPairIfNotExist("Content-Length", std::to_string(m_body.size()));
            return m_header.toString() + m_body;
        }
        return m_header.toString();
    }
}