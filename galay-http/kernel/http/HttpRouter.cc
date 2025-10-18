#include "HttpRouter.h"
#include "HttpConnection.h"
#include "galay-http/protoc/http/HttpBase.h"
#include "galay-http/protoc/http/HttpError.h"
#include "galay-http/utils/HttpLogger.h"
#include "galay-http/utils/HttpUtils.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <galay/utils/System.h>
#include <string>

namespace galay::http
{
    void HttpRouter::mount(const std::string& prefix, const std::string& path, HttpSettings setting)
    {
        HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[HttpRouter] Mount {} -> {}", prefix, path);
        
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

        // 注册精确路由
        m_routes[static_cast<int>(GET)].emplace(routePrefix, 
            std::bind(&HttpRouter::staticFileRoute, this, canonical_path, setting,
                std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

        // 如果最后一个路径段不是 *，则添加 /*
        if (lastSegment != "*") {
            routePrefix += "/*";
        }

        // 注册模板路由（带通配符）
        m_temlate_routes[static_cast<int>(GET)].emplace(routePrefix, 
            std::bind(&HttpRouter::staticFileRoute, this, canonical_path, setting,
                std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
    }

    AsyncResult<std::expected<void, HttpError>> HttpRouter::route(HttpRequest &request, HttpConnection& conn)
    {
        auto& header = request.header();
        HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[HttpRouter] Route {} {}", httpMethodToString(header.method()), header.uri());
        
        // 尝试模板匹配（带参数或通配符）
        HttpParams params;
        //middleware
        // 首先尝试精确匹配（性能更好）
        if(auto it = m_routes[static_cast<int>(header.method())].find(header.uri()); it != m_routes[static_cast<int>(header.method())].end()) {
            HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[HttpRouter] Exact match found");
            // 每个请求使用独立的 waiter，避免并发冲突
            auto waiter = std::make_shared<AsyncWaiter<void, HttpError>>();
            auto co = it->second(request, conn, std::move(params));
            co.then([waiter](){
                waiter->notify({});
            });
            waiter->appendTask(std::move(co));
            return waiter->wait();
        } 
        
        
        for(auto& [template_uri, routes] : m_temlate_routes[static_cast<int>(header.method())]) {
            if(matchRoute(request.header().uri(), template_uri, params)) {
                HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[HttpRouter] Template match found: {}", template_uri);
                // 每个请求使用独立的 waiter，避免并发冲突
                auto waiter = std::make_shared<AsyncWaiter<void, HttpError>>();
                auto co = routes(request, conn, std::move(params));
                co.then([waiter](){
                    waiter->notify({});
                });
                waiter->appendTask(std::move(co));
                return waiter->wait();
            }
        }
        HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[HttpRouter] No route found");
        return {std::unexpected(HttpError(HttpErrorCode::kHttpError_NotFound))};
    }

    Coroutine<nil> HttpRouter::staticFileRoute(std::string path, HttpSettings settings, HttpRequest& request, HttpConnection& conn, HttpParams params)
    {
        // 检查连接是否已关闭
        if (conn.isClosed()) {
            HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[HttpRouter] Connection already closed");
            co_return nil();
        }
        
        auto writer = conn.getResponseWriter({});
        bool has_error = false;
        std::string error_msg;
        
        try {
            // path 已经是规范化的绝对路径（在 mount 中完成）
            std::filesystem::path base_path(path);
            
            // 1. 获取请求的文件相对路径
            std::string relative_file = params["*"];
            
            // 2. 防止路径为空
            if (relative_file.empty()) {
                relative_file = "index.html";  // 默认文件
            }
            
            HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[HttpRouter] Serve file: {}", relative_file);
            
            // 3. 构建完整文件路径
            std::filesystem::path full_path = base_path / relative_file;
            
            // 4. 检查文件是否存在
            if (!std::filesystem::exists(full_path)) {
                // 检查连接状态
                if (conn.isClosed()) {
                    co_return nil();
                }
                auto response = HttpUtils::defaultNotFound();
                co_await writer.reply(response);
                co_return nil();
            }
            
            // 5. 规范化完整路径（解析 .. 等）
            full_path = std::filesystem::canonical(full_path);
            
            // 6. 安全检查：确保规范化后的路径仍在 base_path 下（防止路径遍历攻击）
            auto full_path_str = full_path.string();
            auto base_path_str = base_path.string();
            if (full_path_str.substr(0, base_path_str.length()) != base_path_str) {
                HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[HttpRouter] Path traversal attempt blocked");
                // 检查连接状态
                if (conn.isClosed()) {
                    co_return nil();
                }
                auto response = HttpUtils::defaultForbidden();
                co_await writer.reply(response);
                co_return nil();
            }
            
            // 7. 检查是否是文件（不是目录）
            if (!std::filesystem::is_regular_file(full_path)) {
                // 检查连接状态
                if (conn.isClosed()) {
                    co_return nil();
                }
                auto response = HttpUtils::defaultForbidden();
                co_await writer.reply(response);
                co_return nil();
            }
            
            // 8. 流式发送文件
            auto file_size = std::filesystem::file_size(full_path);
            auto extension = full_path.extension().string().substr(1);
            
            HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[HttpRouter] Streaming file, size: {} bytes", file_size);
            
            // 检查连接状态
            if (conn.isClosed()) {
                HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[HttpRouter] Connection closed before sending");
                co_return nil();
            }
            
            // 准备响应头
            HttpResponseHeader header;
            header.code() = HttpStatusCode::OK_200;
            header.version() = HttpVersion::Http_Version_1_1;
            header.headerPairs().addHeaderPair("Content-Length", std::to_string(file_size));
            header.headerPairs().addHeaderPair("Content-Type", MimeType::convertToMimeType(extension));
            header.headerPairs().addHeaderPair("Transfer-Encoding", "chunked");
            
            // 发送响应头
            auto header_res = co_await writer.replyChunkHeader(header, settings.send_timeout);
            if (!header_res) {
                HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[HttpRouter] Send header failed: {}", header_res.error().message());
                // 发送失败说明连接已被对端关闭，只标记状态，不执行 I/O 操作避免 SIGPIPE
                conn.markClosed();
                co_return nil();
            }
            
            std::vector<char> buffer(settings.chunk_buffer_size);
            std::ifstream file(full_path.string(), std::ios::binary);
            
            if (!file) {
                HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[HttpRouter] Failed to open file");
                // 文件打开失败，需要关闭连接（连接仍然有效）
                if (!conn.isClosed()) {
                    co_await conn.close();
                }
                co_return nil();
            }
            
            size_t total_sent = 0;
            while (file && !file.eof()) {
                // 检查连接是否在传输过程中被关闭
                if (conn.isClosed()) {
                    HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug(
                        "[HttpRouter] Connection closed during transfer at {}/{} bytes", 
                        total_sent, file_size
                    );
                    file.close();
                    co_return nil();
                }
                
                file.read(buffer.data(), settings.chunk_buffer_size);
                size_t bytes_read = file.gcount();
                
                if (bytes_read > 0) {
                    total_sent += bytes_read;
                    bool is_last = (total_sent >= file_size);
                    
                    auto chunk_res = co_await writer.replyChunkData(
                        std::string_view(buffer.data(), bytes_read),
                        is_last,
                        settings.send_timeout
                    );
                    
                    if (!chunk_res) {
                        HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug(
                            "[HttpRouter] Send chunk failed at {}/{} bytes: {}", 
                            total_sent, file_size, chunk_res.error().message()
                        );
                        file.close();
                        // 发送失败说明连接已被对端关闭，只标记状态，不执行 I/O 操作避免 SIGPIPE
                        conn.markClosed();
                        co_return nil();
                    }
                }
            }
            
            file.close();
            
            HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug(
                "[HttpRouter] File sent successfully: {} bytes", total_sent
            );
            
        } catch (const std::filesystem::filesystem_error& e) {
            has_error = true;
            error_msg = std::string("Filesystem error: ") + e.what();
            HttpLogger::getInstance()->getLogger()->getSpdlogger()->error("[HttpRouter] {}", error_msg);
        } catch (const std::exception& e) {
            has_error = true;
            error_msg = std::string("Error: ") + e.what();
            HttpLogger::getInstance()->getLogger()->getSpdlogger()->error("[HttpRouter] {}", error_msg);
        }
        
        // 在 catch 块外处理错误，这里可以使用 co_await
        if (has_error && !conn.isClosed()) {
            HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[HttpRouter] Handling error: {}", error_msg);
            auto response = HttpUtils::defaultInternalServerError();
            auto result = co_await writer.reply(response, settings.send_timeout);
            if (!result) {
                // 如果发送错误响应失败，说明连接已断开，只标记状态
                HttpLogger::getInstance()->getLogger()->getSpdlogger()->error("[HttpRouter] Failed to send error response");
                conn.markClosed();
                co_return nil();
            }
            // 发送错误响应成功，正常关闭连接
            co_await conn.close();
        }
        
        co_return nil();
    }
}
