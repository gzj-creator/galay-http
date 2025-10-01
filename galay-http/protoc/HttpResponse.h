#ifndef GALAY_HTTP_RESPONSE_H
#define GALAY_HTTP_RESPONSE_H 

#include "HttpHeader.h"
#include "HttpBody.h"

namespace galay::http
{
    class HttpResponse 
    {
    public:
        using ptr = std::shared_ptr<HttpResponse>;
        using wptr = std::weak_ptr<HttpResponse>;
        using uptr = std::unique_ptr<HttpResponse>;
        
        HttpResponseHeader& header();
        //移交所有权
        template<HttpBodyType T>
        T getBody();

        //移交所有权
        std::string getBodyStr();

        void setHeader(HttpResponseHeader&& header);
        void setHeader(HttpResponseHeader& header);
        
        template<HttpBodyType T>
        void setBody(T&& body);
        void setBodyStr(std::string&& body);

        std::string toString();
    private:
        HttpResponseHeader m_header;
        std::string m_body;
    };
}


#include "HttpResponse.inl"

#endif