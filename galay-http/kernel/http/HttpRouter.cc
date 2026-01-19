#include "HttpRouter.h"
#include "HttpLog.h"
#include <sstream>
#include <set>
#include <cctype>

namespace galay::http
{

HttpRouter::HttpRouter()
    : m_routeCount(0)
{
}

void HttpRouter::addHandlerInternal(HttpMethod method, const std::string& path, HttpRouteHandler handler)
{
    // 验证路径格式
    std::string error;
    if (!validatePath(path, error)) {
        // 路径格式错误，记录日志并返回
        HTTP_LOG_ERROR("Invalid route path '{}': {}", path, error);
        return;
    }

    if (isFuzzyPattern(path)) {
        // 模糊匹配路由 - 使用Trie树
        auto& root = m_fuzzyRoutes[method];
        if (!root) {
            root = std::make_unique<RouteTrieNode>();
        }

        auto segments = splitPath(path);
        insertRoute(root.get(), segments, handler);
        HTTP_LOG_DEBUG("Added fuzzy route: {} {}", static_cast<int>(method), path);
        m_routeCount++;
    } else {
        // 精确匹配路由 - 使用unordered_map
        // 检查是否已存在（冲突检测）
        auto& methodRoutes = m_exactRoutes[method];
        bool isNewRoute = !methodRoutes.count(path);

        if (!isNewRoute) {
            HTTP_LOG_WARN("Route '{}' for method {} already exists, will be overwritten",
                         path, static_cast<int>(method));
        }

        methodRoutes[path] = handler;
        HTTP_LOG_DEBUG("Added exact route: {} {}", static_cast<int>(method), path);

        // 只有新路由才增加计数
        if (isNewRoute) {
            m_routeCount++;
        }
    }
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

bool HttpRouter::delHandler(HttpMethod method, const std::string& path)
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
            std::string paramName = segment.substr(1);  // 去掉冒号

            auto& child = node->children[":param"];
            if (!child) {
                child = std::make_unique<RouteTrieNode>();
                child->isParam = true;
                child->paramName = paramName;  // 保存参数名在节点上
            } else if (child->paramName != paramName) {
                // 检测冲突：同一位置有不同的参数名
                HTTP_LOG_WARN("Parameter name conflict at same position: '{}' vs '{}', using '{}'",
                             child->paramName, paramName, child->paramName);
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

            // 直接使用节点的参数名保存参数值
            params[paramNode->paramName] = segment;

            auto result = dfs(paramNode, depth + 1);
            if (result) return result;

            // 回溯：如果这条路径不匹配，移除参数
            params.erase(paramNode->paramName);
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

bool HttpRouter::validatePath(const std::string& path, std::string& error) const
{
    // 1. 检查路径是否为空
    if (path.empty()) {
        error = "Path cannot be empty";
        return false;
    }

    // 2. 检查是否以 / 开头
    if (path[0] != '/') {
        error = "Path must start with '/'";
        return false;
    }

    // 3. 检查路径长度
    if (path.length() > 2048) {
        error = "Path is too long (max 2048 characters)";
        return false;
    }

    // 4. 分割路径并检查每个段
    auto segments = splitPath(path);

    if (segments.empty() && path != "/") {
        error = "Invalid path format";
        return false;
    }

    // 5. 检查参数名是否重复
    std::set<std::string> paramNames;
    bool hasWildcard = false;

    for (size_t i = 0; i < segments.size(); ++i) {
        const auto& segment = segments[i];

        // 检查空段
        if (segment.empty()) {
            error = "Path contains empty segment";
            return false;
        }

        // 检查参数节点
        if (segment[0] == ':') {
            if (segment.length() == 1) {
                error = "Parameter name cannot be empty (found ':' without name)";
                return false;
            }

            std::string paramName = segment.substr(1);

            // 检查参数名第一个字符（必须是字母或下划线）
            if (!std::isalpha(paramName[0]) && paramName[0] != '_') {
                error = "Parameter name '" + paramName + "' must start with a letter or underscore";
                return false;
            }

            // 检查参数名是否合法（只能包含字母、数字、下划线）
            for (char c : paramName) {
                if (!std::isalnum(c) && c != '_') {
                    error = "Parameter name '" + paramName + "' contains invalid character '" + std::string(1, c) + "'";
                    return false;
                }
            }

            // 检查参数名是否重复
            if (paramNames.count(paramName)) {
                error = "Duplicate parameter name: '" + paramName + "'";
                return false;
            }
            paramNames.insert(paramName);
        }
        // 检查通配符节点
        else if (segment == "*" || segment == "**") {
            if (hasWildcard) {
                error = "Path can only contain one wildcard";
                return false;
            }

            // 通配符必须是最后一个段
            if (i != segments.size() - 1) {
                error = "Wildcard '" + segment + "' must be the last segment";
                return false;
            }

            hasWildcard = true;
        }
        // 普通段
        else {
            // 检查是否包含非法字符
            for (char c : segment) {
                if (!std::isalnum(c) && c != '-' && c != '_' && c != '.' && c != '~') {
                    error = "Segment '" + segment + "' contains invalid character '" + std::string(1, c) + "'";
                    return false;
                }
            }
        }
    }

    return true;
}

} // namespace galay::http
