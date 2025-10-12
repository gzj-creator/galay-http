#include "HttpRouter.h"
#include "galay-http/protoc/HttpError.h"

namespace galay::http
{

    AsyncResult<std::expected<void, HttpError>> HttpRouter::route(HttpRequest &request, HttpReader &reader, HttpWriter &writer)
    {
        auto& header = request.header();
        // 尝试模板匹配（带参数或通配符）
        HttpParams params;
        //middleware
        // 首先尝试精确匹配（性能更好）
        if(auto it = m_routes[static_cast<int>(header.method())].find(header.uri()); it != m_routes[static_cast<int>(header.method())].end()) {
            auto co = it->second(request, reader, writer, std::move(params));
            co.then([this](){
                m_waiter.notify({});
            });
            m_waiter.appendTask(std::move(co));
            return m_waiter.wait();
        } 
        
        
        for(auto& [template_uri, routes] : m_temlate_routes[static_cast<int>(header.method())]) {
            if(matchRoute(request.header().uri(), template_uri, params)) {
                auto co = routes(request, reader, writer, std::move(params));
                co.then([this](){
                    m_waiter.notify({});
                });
                m_waiter.appendTask(std::move(co));
                return m_waiter.wait();
            }
        }
        return {std::unexpected(HttpError(HttpErrorCode::kHttpError_NotFound))};
    }

   
}
