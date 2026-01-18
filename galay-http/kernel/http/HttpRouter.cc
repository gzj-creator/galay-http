#include "HttpRouter.h"
#include <sstream>
#include <algorithm>

namespace galay::http
{

HttpRouter::HttpRouter()
    : m_routeCount(0)
{
}

void HttpRouter::addHandlerInternal(HttpMethod method, const std::string& path, HttpRouteHandler handler)
{
    if (isFuzzyPattern(path)) {
        // 模糊匹配路由 - 使用Trie树
        auto& root = m_fuzzyRoutes[method];
        if (!root) {
            root = std::make_unique<RouteTrieNode>();
        }

        auto segments = splitPath(path);
        insertRoute(root.get(), segments, handler);
    } else {
        // 精确匹配路由 - 使用unordered_map
        m_exactRoutes[method][path] = handler;
    }

    m_routeCount++;
}

RouteMatch HttpRouter::findHandler(HttpMethod method, const std::string& path)
{
    RouteMatch result;

    // 1. 先尝试精确匹配（O(1)）
    auto methodIt = m_exactRoutes.find(method);
    if (methodIt != m_exactRoutes.end()) {
        auto pathIt = methodIt->second.find(path);
        if (pathIt != methodIt->second.end()) {
            result.handler = &pathIt->second;
            return result;
        }
    }

    // 2. 尝试模糊匹配 - 使用Trie树（O(k)，k为路径段数）
    auto fuzzyIt = m_fuzzyRoutes.find(method);
    if (fuzzyIt != m_fuzzyRoutes.end() && fuzzyIt->second) {
        auto segments = splitPath(path);
        result.handler = searchRoute(fuzzyIt->second.get(), segments, result.params);
    }

    return result;  // 未找到，handler为nullptr
}

bool HttpRouter::removeHandler(HttpMethod method, const std::string& path)
{
    // 尝试从精确匹配中移除
    auto methodIt = m_exactRoutes.find(method);
    if (methodIt != m_exactRoutes.end()) {
        auto removed = methodIt->second.erase(path);
        if (removed > 0) {
            m_routeCount--;
            return true;
        }
    }

    // TODO: 从Trie树中移除路由（较复杂，暂不实现）
    // Trie树的删除需要递归处理，避免留下空节点

    return false;
}

void HttpRouter::clear()
{
    m_exactRoutes.clear();
    m_fuzzyRoutes.clear();
    m_routeCount = 0;
}

size_t HttpRouter::size() const
{
    return m_routeCount;
}

bool HttpRouter::isFuzzyPattern(const std::string& path) const
{
    // 检查是否包含路径参数（:param）或通配符（*）
    return path.find(':') != std::string::npos ||
           path.find('*') != std::string::npos;
}

std::vector<std::string> HttpRouter::splitPath(const std::string& path) const
{
    std::vector<std::string> segments;
    std::stringstream ss(path);
    std::string segment;

    while (std::getline(ss, segment, '/')) {
        if (!segment.empty()) {
            segments.push_back(segment);
        }
    }

    return segments;
}

void HttpRouter::insertRoute(RouteTrieNode* root, const std::vector<std::string>& segments,
                             HttpRouteHandler handler)
{
    RouteTrieNode* node = root;

    for (const auto& segment : segments) {
        // 判断段类型
        if (segment == "*" || segment == "**") {
            // 通配符节点
            auto& child = node->children[segment];
            if (!child) {
                child = std::make_unique<RouteTrieNode>();
                child->isWildcard = true;
            }
            node = child.get();
        } else if (!segment.empty() && segment[0] == ':') {
            // 参数节点（:id）
            // 所有参数节点共享同一个键 ":param"
            auto& child = node->children[":param"];
            if (!child) {
                child = std::make_unique<RouteTrieNode>();
                child->isParam = true;
                child->paramName = segment.substr(1);  // 去掉冒号
            }
            node = child.get();
        } else {
            // 普通节点
            auto& child = node->children[segment];
            if (!child) {
                child = std::make_unique<RouteTrieNode>();
            }
            node = child.get();
        }
    }

    // 标记为路径终点并设置处理函数
    node->isEnd = true;
    node->handler = handler;
}

HttpRouteHandler* HttpRouter::searchRoute(RouteTrieNode* root, const std::vector<std::string>& segments,
                                          std::map<std::string, std::string>& params)
{
    params.clear();

    // 使用递归进行深度优先搜索
    std::function<HttpRouteHandler*(RouteTrieNode*, size_t)> dfs =
        [&](RouteTrieNode* node, size_t depth) -> HttpRouteHandler* {

        // 到达路径末尾
        if (depth == segments.size()) {
            if (node->isEnd) {
                return &node->handler;
            }
            return nullptr;
        }

        const std::string& segment = segments[depth];

        // 1. 优先尝试精确匹配
        auto exactIt = node->children.find(segment);
        if (exactIt != node->children.end()) {
            auto result = dfs(exactIt->second.get(), depth + 1);
            if (result) return result;
        }

        // 2. 尝试参数匹配（:param）
        auto paramIt = node->children.find(":param");
        if (paramIt != node->children.end()) {
            auto* paramNode = paramIt->second.get();
            // 保存参数值
            std::string paramName = paramNode->paramName;
            params[paramName] = segment;

            auto result = dfs(paramNode, depth + 1);
            if (result) return result;

            // 回溯：如果这条路径不匹配，移除参数
            params.erase(paramName);
        }

        // 3. 尝试单段通配符（*）
        auto wildcardIt = node->children.find("*");
        if (wildcardIt != node->children.end()) {
            auto result = dfs(wildcardIt->second.get(), depth + 1);
            if (result) return result;
        }

        // 4. 尝试贪婪通配符（**）- 匹配剩余所有段
        auto greedyIt = node->children.find("**");
        if (greedyIt != node->children.end()) {
            auto* greedyNode = greedyIt->second.get();
            if (greedyNode->isEnd) {
                return &greedyNode->handler;
            }
        }

        return nullptr;
    };

    return dfs(root, 0);
}

} // namespace galay::http
