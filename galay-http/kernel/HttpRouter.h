#ifndef GALAY_HTTP_ROUTER_H
#define GALAY_HTTP_ROUTER_H 

#include "HttpConnection.h"
#include "galay-http/protoc/HttpBase.h"
#include <galay/kernel/async/AsyncFactory.h>

namespace galay::http
{
    class HttpRouter
    {
    public:
        using ptr = std::shared_ptr<HttpRouter>;
        using RouteFunc = std::function<Coroutine<nil>(HttpConnection,AsyncFactory)>;
        
        void addRoute(const std::string& path, RouteFunc function);
        virtual ~HttpRouter() = default;
    };
}

#endif