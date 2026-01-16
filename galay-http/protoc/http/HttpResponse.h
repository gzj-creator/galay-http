#ifndef GALAY_HTTP_RESPONSE_H
#define GALAY_HTTP_RESPONSE_H

#include "HttpHeader.h"
#include "HttpBody.h"
#include <vector>
#include <sys/uio.h>

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

        // 从离散buffer增量解析，返回消耗的字节数，-1表示解析错误，0表示数据不完整
        std::pair<HttpErrorCode, ssize_t> fromIOVec(const std::vector<iovec>& iovecs);

        // 检查是否解析完成（header + body）
        bool isComplete() const;

        // 重置解析状态
        void reset();

    private:
        HttpResponseHeader m_header;
        std::string m_body;
        // body解析状态
        size_t m_contentLength = 0;
        size_t m_bodyParsed = 0;
        size_t m_headerLength = 0;  // header的字节长度
        bool m_headerParsed = false;
    };
}


#include "HttpResponse.inl"

#endif