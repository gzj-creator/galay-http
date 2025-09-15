#ifndef GALAY_HTTP_UTILS_H
#define GALAY_HTTP_UTILS_H 

#include "HttpRequest.h"
#include "HttpResponse.h"

namespace galay::http 
{ 
    class HttpUtils {
    public:
        static HttpResponse defaultBadRequest();
    };
}

#endif