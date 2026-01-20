#include "HttpRouter.h"
#include "HttpLog.h"
#include "FileDescriptor.h"
#include "HttpRange.h"
#include "HttpETag.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include <sstream>
#include <set>
#include <cctype>
#include <fstream>
#include <filesystem>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>

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

    HTTP_LOG_DEBUG("Finding handler for: {} {}", static_cast<int>(method), path);

    // 1. 先尝试精确匹配（O(1)）
    auto methodIt = m_exactRoutes.find(method);
    if (methodIt != m_exactRoutes.end()) {
        HTTP_LOG_DEBUG("Exact routes for method {}: {}", static_cast<int>(method), methodIt->second.size());
        auto pathIt = methodIt->second.find(path);
        if (pathIt != methodIt->second.end()) {
            HTTP_LOG_DEBUG("Found exact match for: {}", path);
            result.handler = &pathIt->second;
            return result;
        }
    }

    // 2. 尝试模糊匹配 - 使用Trie树（O(k)，k为路径段数）
    auto fuzzyIt = m_fuzzyRoutes.find(method);
    if (fuzzyIt != m_fuzzyRoutes.end() && fuzzyIt->second) {
        auto segments = splitPath(path);
        HTTP_LOG_DEBUG("Trying fuzzy match with {} segments", segments.size());
        for (size_t i = 0; i < segments.size(); i++) {
            HTTP_LOG_DEBUG("  Segment[{}]: {}", i, segments[i]);
        }
        result.handler = searchRoute(fuzzyIt->second.get(), segments, result.params);
        if (result.handler) {
            HTTP_LOG_DEBUG("Found fuzzy match for: {}", path);
        } else {
            HTTP_LOG_DEBUG("No fuzzy match found for: {}", path);
        }
    } else {
        HTTP_LOG_DEBUG("No fuzzy routes for method {}", static_cast<int>(method));
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

// ==================== 静态文件服务实现 ====================

void HttpRouter::mount(const std::string& routePrefix, const std::string& dirPath,
                       const StaticFileConfig& config)
{
    namespace fs = std::filesystem;

    // 验证目录是否存在
    if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) {
        HTTP_LOG_ERROR("Mount failed: directory '{}' does not exist", dirPath);
        return;
    }

    // 保存挂载信息
    m_mountedDirs[routePrefix] = dirPath;

    // 创建动态文件处理器
    auto handler = createStaticFileHandler(routePrefix, dirPath, config);

    // 注册通配符路由：routePrefix/**
    std::string wildcardPath = routePrefix;
    if (wildcardPath.back() != '/') {
        wildcardPath += '/';
    }
    wildcardPath += "**";

    // 为 GET 方法注册路由
    addHandler<HttpMethod::GET>(wildcardPath, handler);

    HTTP_LOG_INFO("Mounted directory '{}' to route '{}'", dirPath, routePrefix);
}

void HttpRouter::mountHardly(const std::string& routePrefix, const std::string& dirPath,
                             const StaticFileConfig& config)
{
    namespace fs = std::filesystem;

    // 验证目录是否存在
    if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) {
        HTTP_LOG_ERROR("MountHardly failed: directory '{}' does not exist", dirPath);
        return;
    }

    // 递归遍历目录并注册所有文件
    registerFilesRecursively(routePrefix, dirPath, config, "");

    HTTP_LOG_INFO("Mounted directory '{}' to route '{}' (static mode)", dirPath, routePrefix);
}

HttpRouteHandler HttpRouter::createStaticFileHandler(const std::string& routePrefix,
                                                     const std::string& dirPath,
                                                     const StaticFileConfig& config)
{
    // 捕获 routePrefix、dirPath 和 config，返回一个协程处理器
    return [routePrefix, dirPath, config](HttpConn& conn, HttpRequest req) -> Coroutine {
        namespace fs = std::filesystem;

        // 获取请求的路径参数（通配符匹配的部分）
        std::string requestPath = req.header().uri();

        // 从 URI 中提取相对路径
        // 例如：/static/css/style.css -> css/style.css
        std::string relativePath;
        if (requestPath.size() > routePrefix.size()) {
            // 跳过 routePrefix 和后面的 /
            size_t start = routePrefix.size();
            if (requestPath[start] == '/') {
                start++;
            }
            relativePath = requestPath.substr(start);
        }

        // 构建完整文件路径
        fs::path fullPath = fs::path(dirPath) / relativePath;

        // 安全检查：防止路径遍历攻击
        fs::path canonicalDir = fs::canonical(dirPath);
        fs::path canonicalFile;
        bool fileNotFound = false;
        try {
            canonicalFile = fs::canonical(fullPath);
        } catch (const fs::filesystem_error&) {
            fileNotFound = true;
        }

        if (fileNotFound) {
            // 文件不存在
            HttpResponse response;
            response.header().code() = HttpStatusCode::NotFound_404;
            response.setBodyStr("404 Not Found");
            auto writer = conn.getWriter();
            co_await writer.send(response.toString());
            co_return;
        }

        // 检查文件是否在允许的目录内
        auto [dirIt, fileIt] = std::mismatch(canonicalDir.begin(), canonicalDir.end(),
                                              canonicalFile.begin());
        if (dirIt != canonicalDir.end()) {
            // 路径遍历攻击
            HTTP_LOG_WARN("Path traversal attempt: {}", requestPath);
            HttpResponse response;
            response.header().code() = HttpStatusCode::Forbidden_403;
            response.setBodyStr("403 Forbidden");
            auto writer = conn.getWriter();
            co_await writer.send(response.toString());
            co_return;
        }

        // 检查文件是否存在且是普通文件
        if (!fs::exists(canonicalFile) || !fs::is_regular_file(canonicalFile)) {
            HttpResponse response;
            response.header().code() = HttpStatusCode::NotFound_404;
            response.setBodyStr("404 Not Found");
            auto writer = conn.getWriter();
            co_await writer.send(response.toString());
            co_return;
        }

        // 获取文件大小
        size_t fileSize = fs::file_size(canonicalFile);

        // 设置 Content-Type
        std::string extension = canonicalFile.extension().string();
        std::string ext = extension.empty() ? "" : extension.substr(1);
        std::string mimeType = MimeType::convertToMimeType(ext);

        // 使用配置的传输方式发送文件
        sendFileContent(conn, canonicalFile.string(), fileSize, mimeType, config);
        co_return;
    };
}

void HttpRouter::registerFilesRecursively(const std::string& routePrefix,
                                          const std::string& dirPath,
                                          const StaticFileConfig& config,
                                          const std::string& currentPath)
{
    namespace fs = std::filesystem;

    fs::path fullPath = fs::path(dirPath) / currentPath;

    try {
        for (const auto& entry : fs::directory_iterator(fullPath)) {
            std::string entryName = entry.path().filename().string();
            std::string relativePath = currentPath.empty() ? entryName : currentPath + "/" + entryName;

            if (entry.is_directory()) {
                // 递归处理子目录
                registerFilesRecursively(routePrefix, dirPath, config, relativePath);
            } else if (entry.is_regular_file()) {
                // 为文件创建路由
                std::string routePath = routePrefix;
                if (routePath.back() != '/') {
                    routePath += '/';
                }
                routePath += relativePath;

                // 创建文件处理器
                std::string filePath = entry.path().string();
                auto handler = createSingleFileHandler(filePath, config);

                // 注册路由
                addHandler<HttpMethod::GET>(routePath, handler);
            }
        }
    } catch (const fs::filesystem_error& e) {
        HTTP_LOG_ERROR("Error reading directory '{}': {}", fullPath.string(), e.what());
    }
}

HttpRouteHandler HttpRouter::createSingleFileHandler(const std::string& filePath,
                                                     const StaticFileConfig& config)
{
    // 捕获文件路径和配置
    return [filePath, config](HttpConn& conn, HttpRequest req) -> Coroutine {
        namespace fs = std::filesystem;

        // 检查文件是否存在
        if (!fs::exists(filePath) || !fs::is_regular_file(filePath)) {
            HttpResponse response;
            response.header().code() = HttpStatusCode::NotFound_404;
            response.setBodyStr("404 Not Found");
            auto writer = conn.getWriter();
            co_await writer.send(response.toString());
            co_return;
        }

        // 获取文件大小
        size_t fileSize = fs::file_size(filePath);

        // 设置 Content-Type
        fs::path path(filePath);
        std::string extension = path.extension().string();
        std::string ext = extension.empty() ? "" : extension.substr(1);
        std::string mimeType = MimeType::convertToMimeType(ext);

        // 使用配置的传输方式发送文件
        sendFileContent(conn, filePath, fileSize, mimeType, config);
        co_return;
    };
}

// ==================== 文件传输实现 ====================

Coroutine HttpRouter::sendFileContent(HttpConn& conn,
                                      const std::string& filePath,
                                      size_t fileSize,
                                      const std::string& mimeType,
                                      const StaticFileConfig& config)
{
    namespace fs = std::filesystem;

    // 生成 ETag
    std::string etag = ETagGenerator::generate(filePath);
    std::time_t lastModified = std::time(nullptr);  // 简化：使用当前时间

    std::string lastModifiedStr = ETagGenerator::formatHttpDate(lastModified);

    // 获取请求头（需要从 HttpRequest 中获取，这里暂时跳过）
    // TODO: 从 HttpRequest 中提取 Range 和 If-None-Match 头

    // 根据配置决定传输模式
    FileTransferMode mode = config.decideTransferMode(fileSize);

    // 构建响应头
    HttpResponse response;
    response.header().code() = HttpStatusCode::OK_200;
    response.header().headerPairs().addHeaderPair("Content-Type", mimeType);
    response.header().headerPairs().addHeaderPair("ETag", etag);
    response.header().headerPairs().addHeaderPair("Last-Modified", lastModifiedStr);
    response.header().headerPairs().addHeaderPair("Accept-Ranges", "bytes");

    auto writer = conn.getWriter();

    switch (mode) {
        case FileTransferMode::MEMORY: {
            // 内存模式：将文件完整读入内存后发送
            HTTP_LOG_DEBUG("Using MEMORY mode for file: {} (size: {})", filePath, fileSize);

            std::ifstream file(filePath, std::ios::binary);
            if (!file) {
                response.header().code() = HttpStatusCode::InternalServerError_500;
                response.setBodyStr("500 Internal Server Error");
                co_await writer.send(response.toString());
                co_return;
            }

            std::string content(fileSize, '\0');
            file.read(&content[0], fileSize);
            response.setBodyStr(std::move(content));

            auto result = co_await writer.send(response.toString());
            if (!result) {
                HTTP_LOG_ERROR("Failed to send response: {}", result.error().message());
            }
            break;
        }

        case FileTransferMode::CHUNK: {
            // Chunk 模式：使用 HTTP chunked 编码分块传输
            HTTP_LOG_DEBUG("Using CHUNK mode for file: {} (size: {})", filePath, fileSize);

            response.header().headerPairs().addHeaderPair("Transfer-Encoding", "chunked");

            // 发送响应头（只发送头部，不包含 body）
            std::string headerStr = response.header().toString();
            auto headerResult = co_await writer.send(std::move(headerStr));
            if (!headerResult) {
                HTTP_LOG_ERROR("Failed to send response header: {}", headerResult.error().message());
                co_return;
            }

            // 使用 RAII 管理文件描述符
            FileDescriptor fd;
            bool openSuccess = false;
            try {
                fd.open(filePath.c_str(), O_RDONLY);
                openSuccess = true;
            } catch (const std::system_error& e) {
                HTTP_LOG_ERROR("Failed to open file for CHUNK mode: {} - {}", filePath, e.what());
            }

            if (!openSuccess) {
                // 发送空 chunk 结束
                co_await writer.sendChunk("", true);
                co_return;
            }

            // 分块读取并发送
            size_t chunkSize = config.getChunkSize();
            std::vector<char> buffer(chunkSize);
            ssize_t bytesRead;
            bool hasError = false;

            while ((bytesRead = read(fd.get(), buffer.data(), chunkSize)) > 0) {
                std::string chunk(buffer.data(), bytesRead);
                auto result = co_await writer.sendChunk(chunk, false);
                if (!result) {
                    HTTP_LOG_ERROR("Failed to send chunk: {}", result.error().message());
                    hasError = true;
                    break;
                }
            }

            // 检查读取错误
            if (bytesRead < 0) {
                HTTP_LOG_ERROR("Failed to read file: {}", strerror(errno));
                hasError = true;
            }

            // 发送最后一个空 chunk
            if (!hasError) {
                co_await writer.sendChunk("", true);
            }

            // fd 会在作用域结束时自动关闭
            break;
        }

        case FileTransferMode::SENDFILE: {
            // SendFile 模式：使用零拷贝 sendfile 系统调用
            HTTP_LOG_DEBUG("Using SENDFILE mode for file: {} (size: {})", filePath, fileSize);

            response.header().headerPairs().addHeaderPair("Content-Length", std::to_string(fileSize));

            // 发送响应头（只发送头部，不包含 body）
            std::string headerStr = response.header().toString();
            auto headerResult = co_await writer.send(std::move(headerStr));
            if (!headerResult) {
                HTTP_LOG_ERROR("Failed to send response header: {}", headerResult.error().message());
                co_return;
            }

            // 使用 RAII 管理文件描述符
            FileDescriptor fd;
            try {
                fd.open(filePath.c_str(), O_RDONLY);
            } catch (const std::system_error& e) {
                HTTP_LOG_ERROR("Failed to open file for SENDFILE mode: {} - {}", filePath, e.what());
                co_return;
            }

            // 使用 sendfile 零拷贝发送文件内容
            off_t offset = 0;
            size_t remaining = fileSize;
            size_t sendfileChunkSize = config.getSendFileChunkSize();

            while (remaining > 0) {
                size_t toSend = std::min(remaining, sendfileChunkSize);
                auto result = co_await conn.socket().sendfile(fd.get(), offset, toSend);

                if (!result) {
                    HTTP_LOG_ERROR("Sendfile failed: {}", result.error().message());
                    break;
                }

                size_t sent = result.value();
                if (sent == 0) {
                    HTTP_LOG_WARN("Sendfile returned 0, connection may be closed");
                    break;
                }

                offset += sent;
                remaining -= sent;
            }

            // fd 会在作用域结束时自动关闭
            break;
        }

        case FileTransferMode::AUTO:
            // AUTO 模式应该在 decideTransferMode 中已经被转换为具体模式
            HTTP_LOG_ERROR("AUTO mode should not reach here");
            break;
    }

    co_return;
}

} // namespace galay::http

