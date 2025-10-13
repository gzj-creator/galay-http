#include "HttpRouter.h"
#include "galay-http/kernel/HttpConnection.h"
#include "galay-http/protoc/HttpBase.h"
#include "galay-http/protoc/HttpError.h"
#include "galay-http/utils/HttpLogger.h"
#include "galay-http/utils/HttpUtils.h"
#include <filesystem>
#include <galay/utils/System.h>
#include <string>

namespace galay::http
{
    void HttpRouter::mount(const std::string& prefix, const std::string& path)
    {
        // 1. 规范化路由前缀
        std::string routePrefix = prefix;
        
        // 去除尾部的斜杠（如果有）
        while (!routePrefix.empty() && routePrefix.back() == '/') {
            routePrefix.pop_back();
        }
        
        // 检查最后一个路径段是否为 *
        size_t lastSlash = routePrefix.find_last_of('/');
        std::string lastSegment;
        if (lastSlash != std::string::npos) {
            lastSegment = routePrefix.substr(lastSlash + 1);
        } else {
            lastSegment = routePrefix;
        }
        
        // 如果最后一个路径段不是 *，则添加 /*
        if (lastSegment != "*") {
            routePrefix += "/*";
        }
        
        // 2. 规范化基础路径并验证
        std::filesystem::path base_path(path);
        
        // 检查路径是否存在
        if (!std::filesystem::exists(base_path)) {
            throw std::runtime_error("Mount path does not exist: " + path);
        }
        
        // 检查是否为目录
        if (!std::filesystem::is_directory(base_path)) {
            throw std::runtime_error("Mount path is not a directory: " + path);
        }
        
        // 规范化为绝对路径
        base_path = std::filesystem::canonical(std::filesystem::absolute(base_path));
        std::string canonical_path = base_path.string();
        
        // 3. 绑定静态文件路由处理函数，传递规范化后的路径
        auto handler = std::bind(&HttpRouter::staticFileRoute, this, canonical_path, 
                                std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
        m_temlate_routes[static_cast<int>(GET)].emplace(routePrefix, handler);
    }

    AsyncResult<std::expected<void, HttpError>> HttpRouter::route(HttpRequest &request, HttpConnection& conn)
    {
        auto& header = request.header();
        // 尝试模板匹配（带参数或通配符）
        HttpParams params;
        //middleware
        // 首先尝试精确匹配（性能更好）
        if(auto it = m_routes[static_cast<int>(header.method())].find(header.uri()); it != m_routes[static_cast<int>(header.method())].end()) {
            auto co = it->second(request, conn, std::move(params));
            co.then([this](){
                m_waiter.notify({});
            });
            m_waiter.appendTask(std::move(co));
            return m_waiter.wait();
        } 
        
        
        for(auto& [template_uri, routes] : m_temlate_routes[static_cast<int>(header.method())]) {
            if(matchRoute(request.header().uri(), template_uri, params)) {
                auto co = routes(request, conn, std::move(params));
                co.then([this](){
                    m_waiter.notify({});
                });
                m_waiter.appendTask(std::move(co));
                return m_waiter.wait();
            }
        }
        return {std::unexpected(HttpError(HttpErrorCode::kHttpError_NotFound))};
    }

    Coroutine<nil> HttpRouter::staticFileRoute(std::string path, HttpRequest& request, HttpConnection& conn, HttpParams params)
    {
        auto writer = conn.getResponseWriter({});
        HttpResponse response;
        
        try {
            // path 已经是规范化的绝对路径（在 mount 中完成）
            std::filesystem::path base_path(path);
            
            // 1. 获取请求的文件相对路径
            std::string relative_file = params["*"];
            
            // 2. 防止路径为空
            if (relative_file.empty()) {
                relative_file = "index.html";  // 默认文件
            }
            
            // 3. 构建完整文件路径
            std::filesystem::path full_path = base_path / relative_file;
            
            // 4. 检查文件是否存在
            if (!std::filesystem::exists(full_path)) {
                response = HttpUtils::defaultNotFound();
            } else {
                // 5. 规范化完整路径（解析 .. 等）
                full_path = std::filesystem::canonical(full_path);
                
                // 6. 安全检查：确保规范化后的路径仍在 base_path 下（防止路径遍历攻击）
                auto full_path_str = full_path.string();
                auto base_path_str = base_path.string();
                if (full_path_str.substr(0, base_path_str.length()) != base_path_str) {
                    // 路径遍历攻击！返回 403 Forbidden
                    response = HttpUtils::defaultForbidden();
                } 
                // 7. 检查是否是文件（不是目录）
                else if (!std::filesystem::is_regular_file(full_path)) {
                    response = HttpUtils::defaultForbidden();
                } else {
                    // 8. 读取文件内容
                    try {
                        auto content = utils::zeroReadFile(full_path.string());
                        auto extension = full_path.extension().string().substr(1);
#ifdef ENABLE_DEBUG
                        HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[Ext: {}]", extension);
#endif
                        response = HttpUtils::defaultOk(extension, std::move(content));
                    } catch (std::exception& e) {
                        response = HttpUtils::defaultInternalServerError();
                    }
                }
            }
            
        } catch (const std::filesystem::filesystem_error& e) {
            // 文件系统错误（如路径不存在、权限问题等）
            response = HttpUtils::defaultNotFound();
        } catch (const std::exception& e) {
            // 其他异常
            response = HttpUtils::defaultInternalServerError();
        }
        
        // 发送响应
        auto resp_res = co_await writer.reply(response);
        if(!resp_res) {
            HttpLogger::getInstance()->getLogger()->getSpdlogger()->error("staticFileRoute error: {}", resp_res.error().message());
            co_return nil();
        }
        co_return nil();
    }
}
