#ifndef GALAY_HTTPS_ROUTER_H
#define GALAY_HTTPS_ROUTER_H 

#include "HttpsConnection.h"
#include "galay-http/protoc/http/HttpBase.h"
#include "galay-http/protoc/http/HttpError.h"
#include <functional>
#include <memory>

namespace galay::http
{

    using HttpsParams = std::unordered_map<std::string, std::string>;
    using HttpsFunc = std::function<Coroutine<nil>(HttpRequest&, HttpsConnection&, HttpsParams)>;
    using HttpsRouteMap = std::unordered_map<std::string, HttpsFunc>;
    
    class HttpsRouter
    {
    public:
        using ptr = std::shared_ptr<HttpsRouter>;
        /*
            @param prefix   路由前缀，用于匹配路由
            @param path     文件路径，用于读取文件
            @param callback 文件传输进度回调函数（可选）
            @param settings HTTP 设置参数（可选）
        */
        void mount(const std::string& prefix, const std::string& path, 
                   FileTransferProgressCallback callback = nullptr,
                   HttpSettings settings = {});

        template <HttpMethod ...Methods>
        void addRoute(const std::string& path, HttpsFunc function);
        template <HttpMethod ...Methods>
        void addRoute(const HttpsRouteMap& map);
        AsyncResult<std::expected<void, HttpError>> 
            route(HttpRequest& request, HttpsConnection& conn);
        virtual ~HttpsRouter() = default;
    private:
        Coroutine<nil> staticFileRoute(std::string path, FileTransferProgressCallback callback, 
                                        HttpSettings settings, HttpRequest& request, 
                                        HttpsConnection& conn, HttpsParams params);
        

        /**
         * 判断路径是否为模板路径（包含参数或通配符）
         * @param path 路由路径
         * @return 如果路径包含 {param} 或 * 则返回true，否则返回false
         * 
         * 模板路径示例：
         * - "/user/{id}" - 包含参数捕获
         * - "/files/ *" - 包含通配符（星号）
         * - "/api/{version}/user/{id}" - 包含多个参数
         * 
         * 非模板路径示例：
         * - "/api/users" - 精确匹配路径
         * - "/hello/world" - 精确匹配路径
         */
        inline bool isTemplatePath(const std::string& path) const {
            // 检查是否包含参数捕获 {param}
            if (path.find('{') != std::string::npos && path.find('}') != std::string::npos) {
                return true;
            }
            // 检查是否包含通配符 *
            if (path.find('*') != std::string::npos) {
                return true;
            }
            return false;
        }

        /**
         * 判断URI是否满足模板，并提取参数
         * @param uri 请求的URI，如 "/hello/world"
         * @param pattern 路由模板，如 "/hello/ *" 或 "/hello/{id}"
         * @param params 输出参数，存储提取的路径参数
         * @return 如果URI匹配模板返回true，否则返回false
         * 
         * 模板规则：
         * - 星号 : 通配符，可以匹配一个或多个路径段
         * - {name} : 参数捕获，匹配一个路径段并将值存储在params中
         * - 普通字符串 : 精确匹配
         * 
         * 优化特性：
         * - 零拷贝：直接在原字符串上操作，不进行分割
         * - 避免临时对象：使用指针/索引遍历，减少内存分配
         * - 高效比较：使用memcmp进行快速内存比较
         */
        bool matchRoute(const std::string& uri, const std::string& pattern, 
                        HttpsParams& params) {
            params.clear();
            return matchPath(uri, 0, pattern, 0, params);
        }

        // 跳过前导斜杠
        inline size_t skipSlashes(const std::string& str, size_t pos) const {
            while (pos < str.size() && str[pos] == '/') {
                ++pos;
            }
            return pos;
        }
        
        // 查找下一个斜杠的位置
        inline size_t findNextSlash(const std::string& str, size_t pos) const {
            while (pos < str.size() && str[pos] != '/') {
                ++pos;
            }
            return pos;
        }
        
        // 比较两个字符串段是否相等（不创建子串）
        inline bool segmentEquals(const std::string& str, size_t start, size_t end,
                                const char* target, size_t targetLen) const {
            if (end - start != targetLen) {
                return false;
            }
            return std::memcmp(&str[start], target, targetLen) == 0;
        }
        
        // 直接在字符串上进行匹配，避免分割
        bool matchPath(const std::string& uri, size_t uriPos,
                    const std::string& pattern, size_t patternPos,
                    HttpsParams& params) const {
            // 跳过前导斜杠
            uriPos = skipSlashes(uri, uriPos);
            patternPos = skipSlashes(pattern, patternPos);
            
            // 都到达末尾，匹配成功
            if (uriPos >= uri.size() && patternPos >= pattern.size()) {
                return true;
            }
            
            // 模板已结束但URI还有剩余
            if (patternPos >= pattern.size()) {
                return false;
            }
            
            // URI已结束但模板还有剩余
            if (uriPos >= uri.size()) {
                return false;
            }
            
            // 找到当前段的结束位置
            size_t uriSegEnd = findNextSlash(uri, uriPos);
            size_t patternSegEnd = findNextSlash(pattern, patternPos);
            
            // 获取当前模板段的长度
            size_t patternSegLen = patternSegEnd - patternPos;
            
            // 处理通配符 *
            if (patternSegLen == 1 && pattern[patternPos] == '*') {
                // * 匹配一个或多个段
                // 尝试匹配不同数量的段
                size_t uriNextPos = uriSegEnd;
                
                // 尝试从当前位置一直到末尾的所有可能
                while (true) {
                    std::unordered_map<std::string, std::string> tempParams = params;
                    if (matchPath(uri, uriNextPos, pattern, patternSegEnd, tempParams)) {
                        // 提取通配符匹配的内容
                        std::string wildcardContent(&uri[uriPos], uriNextPos - uriPos);
                        // 移除尾部的斜杠（如果有）
                        while (!wildcardContent.empty() && wildcardContent.back() == '/') {
                            wildcardContent.pop_back();
                        }
                        tempParams["*"] = wildcardContent;
                        params = tempParams;
                        return true;
                    }
                    
                    // 如果到达URI末尾，停止尝试
                    if (uriNextPos >= uri.size()) {
                        break;
                    }
                    
                    // 跳到下一个段
                    uriNextPos = skipSlashes(uri, uriNextPos);
                    if (uriNextPos >= uri.size()) {
                        break;
                    }
                    uriNextPos = findNextSlash(uri, uriNextPos);
                }
                return false;
            }
            
            // 处理参数 {name}
            if (patternSegLen > 2 && pattern[patternPos] == '{' && 
                pattern[patternSegEnd - 1] == '}') {
                // 提取参数名（避免substr）
                std::string paramName(&pattern[patternPos + 1], patternSegLen - 2);
                // 提取参数值（避免substr）
                std::string paramValue(&uri[uriPos], uriSegEnd - uriPos);
                params[paramName] = paramValue;
                
                return matchPath(uri, uriSegEnd, pattern, patternSegEnd, params);
            }
            
            // 普通字符串精确匹配
            size_t uriSegLen = uriSegEnd - uriPos;
            if (uriSegLen == patternSegLen && 
                std::memcmp(&uri[uriPos], &pattern[patternPos], patternSegLen) == 0) {
                return matchPath(uri, uriSegEnd, pattern, patternSegEnd, params);
            }
            
            return false;
        }
    private:
        // 移除了 m_waiter 成员变量 - 每个请求现在使用独立的局部 waiter，避免并发冲突
        std::array<std::unordered_map<std::string, HttpsFunc>, static_cast<int>(HttpMethod::Http_Method_Size)> m_routes;
        std::array<std::unordered_map<std::string, HttpsFunc>, static_cast<int>(HttpMethod::Http_Method_Size)> m_temlate_routes;
    };


    template <HttpMethod... Methods>
    inline void HttpsRouter::addRoute(const std::string& path, HttpsFunc function)
    {
        // 判断路径是否为模板路径
        if (isTemplatePath(path)) {
            // 模板路径（包含参数或通配符），添加到模板路由表
            ([&](){
                this->m_temlate_routes[static_cast<int>(Methods)].emplace(path, function);
            }(), ...);
        } else {
            // 精确匹配路径，添加到普通路由表
            ([&](){
                this->m_routes[static_cast<int>(Methods)].emplace(path, function);
            }(), ...);
        }
    }

    template <HttpMethod ...Methods>
    inline void HttpsRouter::addRoute(const HttpsRouteMap& map)
    {
        // 遍历 map 中的每个路由，根据路径类型分发
        for (const auto& [path, func] : map) {
            if (isTemplatePath(path)) {
                // 模板路径，添加到模板路由表
                ([&](){
                    this->m_temlate_routes[static_cast<int>(Methods)].emplace(path, func);
                }(), ...);
            } else {
                // 精确匹配路径，添加到普通路由表
                ([&](){
                    this->m_routes[static_cast<int>(Methods)].emplace(path, func);
                }(), ...);
            }
        }
    }

}

#endif

