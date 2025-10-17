#ifndef GALAY_HTTP_BODY_H
#define GALAY_HTTP_BODY_H 

#include "HttpBase.h"
#include <concepts>

namespace galay::http 
{
    class HttpBody {
    public:
        virtual ~HttpBody() = default;
        virtual std::string contentType() = 0;
        virtual bool fromString(std::string&& str) = 0;
        virtual std::string toString() = 0;
    }; 

    class PlainBody: public HttpBody 
    {
    public:
        std::string contentType() override { return "text/plain"; }
        bool fromString(std::string&& str) override;
        //移交所有权
        std::string toString() override;
    private:
        std::string m_body;
    };



    template<typename T>
    concept HttpBodyType = 
        std::derived_from<T, HttpBody> &&
        std::is_default_constructible_v<T> &&
        std::is_move_assignable_v<T> &&
        std::is_move_constructible_v<T>;

}

#endif