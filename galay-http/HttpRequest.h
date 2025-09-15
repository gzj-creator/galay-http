#ifndef GALAY_HTTP_REQUEST_H
#define GALAY_HTTP_REQUEST_H 

#include "HttpHeader.h"
#include "HttpBody.h"

namespace galay::http 
{ 

    //处理普通 request
    class HttpRequest
    {
    public:
        using ptr = std::shared_ptr<HttpRequest>;
        using wptr = std::weak_ptr<HttpRequest>;
        using uptr = std::unique_ptr<HttpRequest>;
        
        HttpRequestHeader& header();
        //移交所有权
        template<HttpBodyType T>
        T getBody();

        //移交所有权
        std::string getBodyStr();

        void setHeader(HttpRequestHeader&& header);
        void setHeader(HttpRequestHeader& header);
        
        template<HttpBodyType T>
        void setBody(T&& body);
        void setBodyStr(std::string&& body);

        std::string toString();
        
    private:
        std::string m_body;
        HttpRequestHeader m_header;
    };

}

#include "HttpRequest.inl"

#endif