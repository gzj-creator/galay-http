#ifndef GALAY_HTTP_ROUTER_H
#define GALAY_HTTP_ROUTER_H

#include "HttpConn.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpBase.h"
#include "galay-kernel/kernel/Coroutine.h"
#include <functional>
#include <unordered_map>
#include <string>
#include <memory>
#include <vector>
#include <map>

namespace galay::http
{

using namespace galay::kernel;

/**
 * @brief HTTP路由处理器类型
 * @details 用户提供的处理函数，接收HttpConn引用和HttpRequest
 */
using HttpRouteHandler = std::function<Coroutine(HttpConn&, HttpRequest)>;

/**
 * @brief 路由匹配结果
 */
struct RouteMatch
{
    HttpRouteHandler* handler = nullptr;
    std::map<std::string, std::string> params;  // 路径参数，例如 /user/:id 中的 id
};

/**
 * @brief Trie树节点（用于路由树）
 */
struct RouteTrieNode
{
    std::unordered_map<std::string, std::unique_ptr<RouteTrieNode>> children;  // 子节点
    HttpRouteHandler handler;                                                   // 处理函数
    bool isEnd = false;                                                         // 是否为路径终点
    std::string paramName;                                                      // 参数名（如果是参数节点）
    bool isParam = false;                                                       // 是否为参数节点（:id）
    bool isWildcard = false;                                                    // 是否为通配符节点（*）
};

/**
 * @brief HTTP路由器类（Drogon策略实现）
 * @details 提供基于HTTP方法和路径的路由功能，使用混合策略：
 *          1. 精确匹配：使用 unordered_map，O(1) 查找
 *          2. 模糊匹配：使用 Trie树，O(k) 查找（k为路径段数）
 *
 *          支持的路径模式：
 *          - 精确路径：/api/users
 *          - 路径参数：/user/:id 匹配 /user/123，提取 id=123
 *          - 通配符：/static/* 匹配 /static/任意单段
 *          - 贪婪通配符：/files/** 匹配 /files/任意多段
 */
class HttpRouter
{
public:
    /**
     * @brief 构造函数
     */
    HttpRouter();

    /**
     * @brief 析构函数
     */
    ~HttpRouter() = default;

    // 禁用拷贝
    HttpRouter(const HttpRouter&) = delete;
    HttpRouter& operator=(const HttpRouter&) = delete;

    // 启用移动
    HttpRouter(HttpRouter&&) = default;
    HttpRouter& operator=(HttpRouter&&) = default;

    /**
     * @brief 添加路由处理器（模板方法）
     * @tparam Methods HTTP方法类型（可变参数模板）
     * @param path 路由路径，支持：
     *             - 精确路径：/api/users
     *             - 路径参数：/user/:id 或 /user/:id/posts/:postId
     *             - 通配符：/static/* 或 /files/**
     * @param handler 处理函数
     * @details 支持多个HTTP方法，例如: addHandler<HttpMethod::GET, HttpMethod::POST>("/api", handler)
     */
    template<HttpMethod... Methods>
    void addHandler(const std::string& path, HttpRouteHandler handler) {
        (addHandlerInternal(Methods, path, handler), ...);
    }

    /**
     * @brief 查找路由处理器
     * @param method HTTP方法
     * @param path 请求路径
     * @return RouteMatch 匹配结果，包含处理器和路径参数
     */
    RouteMatch findHandler(HttpMethod method, const std::string& path);

    /**
     * @brief 移除路由处理器
     * @param method HTTP方法
     * @param path 路由路径
     * @return 是否成功移除
     */
    bool removeHandler(HttpMethod method, const std::string& path);

    /**
     * @brief 清空所有路由
     */
    void clear();

    /**
     * @brief 获取所有已注册的路由数量
     * @return 路由数量
     */
    size_t size() const;

private:
    /**
     * @brief 内部添加路由处理器的实现
     * @param method HTTP方法
     * @param path 路由路径
     * @param handler 处理函数
     */
    void addHandlerInternal(HttpMethod method, const std::string& path, HttpRouteHandler handler);

    /**
     * @brief 判断路径是否为模糊匹配模式
     * @param path 路径
     * @return 是否为模糊匹配
     */
    bool isFuzzyPattern(const std::string& path) const;

    /**
     * @brief 分割路径为段
     * @param path 路径
     * @return 路径段列表
     */
    std::vector<std::string> splitPath(const std::string& path) const;

    /**
     * @brief 在Trie树中插入路由
     * @param root Trie树根节点
     * @param segments 路径段列表
     * @param handler 处理函数
     */
    void insertRoute(RouteTrieNode* root, const std::vector<std::string>& segments,
                     HttpRouteHandler handler);

    /**
     * @brief 在Trie树中查找路由
     * @param root Trie树根节点
     * @param segments 路径段列表
     * @param params 输出参数：提取的路径参数
     * @return 处理函数指针，未找到返回nullptr
     */
    HttpRouteHandler* searchRoute(RouteTrieNode* root, const std::vector<std::string>& segments,
                                  std::map<std::string, std::string>& params);

private:
    // 精确匹配路由表：HttpMethod -> (path -> handler)
    // 使用 unordered_map 实现 O(1) 查找
    std::unordered_map<HttpMethod, std::unordered_map<std::string, HttpRouteHandler>> m_exactRoutes;

    // 模糊匹配路由树：HttpMethod -> Trie树根节点
    // 使用 Trie树 实现 O(k) 查找（k为路径段数）
    std::unordered_map<HttpMethod, std::unique_ptr<RouteTrieNode>> m_fuzzyRoutes;

    // 路由计数
    size_t m_routeCount = 0;
};

} // namespace galay::http

#endif // GALAY_HTTP_ROUTER_H
