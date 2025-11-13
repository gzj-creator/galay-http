#include "HttpsRouter.h"
#include "HttpsConnection.h"
#include "HttpsWriter.h"
#include "galay-http/protoc/http/HttpBase.h"
#include "galay-http/protoc/http/HttpError.h"
#include "galay-http/utils/HttpsDebugLog.h"
#include "galay-http/utils/HttpUtils.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <chrono>     // for time measurement
#include <galay/utils/System.h>
#include <string>
#ifdef __linux__
#include <algorithm>  // for std::min
#include <fcntl.h>
#include <unistd.h>
#include <cstring>  // for strerror
#endif

namespace galay::http
{
    void HttpsRouter::mount(const std::string& prefix, const std::string& path, 
                           FileTransferProgressCallback callback, HttpSettings settings)
    {
        HTTPS_LOG_DEBUG("[HttpsRouter] Mount {} -> {}", prefix, path);
        
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
            std::bind(&HttpsRouter::staticFileRoute, this, canonical_path, callback, settings,
                std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

        // 如果最后一个路径段不是 *，则添加 /*
        if (lastSegment != "*") {
            routePrefix += "/*";
        }

        // 注册模板路由（带通配符）
        m_temlate_routes[static_cast<int>(GET)].emplace(routePrefix, 
            std::bind(&HttpsRouter::staticFileRoute, this, canonical_path, callback, settings,
                std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
    }

    AsyncResult<std::expected<void, HttpError>> HttpsRouter::route(HttpRequest &request, HttpsConnection& conn)
    {
        auto& header = request.header();
        HTTPS_LOG_DEBUG("[HttpsRouter] Route {} {}", httpMethodToString(header.method()), header.uri());
        
        // 尝试模板匹配（带参数或通配符）
        HttpsParams params;
        //middleware
        // 首先尝试精确匹配（性能更好）
        if(auto it = m_routes[static_cast<int>(header.method())].find(header.uri()); it != m_routes[static_cast<int>(header.method())].end()) {
            HTTPS_LOG_DEBUG("[HttpsRouter] Exact match found");
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
                HTTPS_LOG_DEBUG("[HttpsRouter] Template match found: {}", template_uri);
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
        HTTPS_LOG_DEBUG("[HttpsRouter] No route found");
        return {std::unexpected(HttpError(HttpErrorCode::kHttpError_NotFound))};
    }

    Coroutine<nil> HttpsRouter::staticFileRoute(std::string path, FileTransferProgressCallback callback, 
                                                HttpSettings settings, HttpRequest& request, 
                                                HttpsConnection& conn, HttpsParams params)
    {
        // 检查连接是否已关闭
        if (conn.isClosed()) {
            HTTPS_LOG_DEBUG("[HttpsRouter] Connection already closed");
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
            
            HTTPS_LOG_DEBUG("[HttpsRouter] Serve file: {}", relative_file);
            
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
                HTTPS_LOG_DEBUG("[HttpsRouter] Path traversal attempt blocked");
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
            
            // 8. 发送文件
            auto file_size = std::filesystem::file_size(full_path);
            auto extension = full_path.extension().string().substr(1);
            
            // 解析 Range 请求头（断点续传支持）
            bool is_range_request = false;
            size_t range_start = 0;
            size_t range_end = file_size - 1;
            
            if (settings.support_range && request.header().headerPairs().hasKey("Range")) {
                std::string range_header = request.header().headerPairs().getValue("Range");
                HTTPS_LOG_DEBUG(
                    "[HttpsRouter] Range request: {}", range_header
                );
                
                // 解析 "bytes=start-end" 格式
                if (range_header.substr(0, 6) == "bytes=") {
                    std::string range_spec = range_header.substr(6);
                    size_t dash_pos = range_spec.find('-');
                    
                    if (dash_pos != std::string::npos) {
                        std::string start_str = range_spec.substr(0, dash_pos);
                        std::string end_str = range_spec.substr(dash_pos + 1);
                        
                        try {
                            if (!start_str.empty()) {
                                range_start = std::stoull(start_str);
                            }
                            if (!end_str.empty()) {
                                range_end = std::stoull(end_str);
                            } else {
                                range_end = file_size - 1;
                            }
                            
                            // 验证范围
                            if (range_start < file_size && range_end < file_size && range_start <= range_end) {
                                is_range_request = true;
                                HTTPS_LOG_DEBUG(
                                    "[HttpsRouter] Valid range: {}-{}/{}", 
                                    range_start, range_end, file_size
                                );
                            } else {
                                HTTPS_LOG_DEBUG(
                                    "[HttpsRouter] Invalid range: {}-{}/{}", 
                                    range_start, range_end, file_size
                                );
                                // 返回 416 Range Not Satisfiable
                                if (!conn.isClosed()) {
                                    HttpResponse response;
                                    response.header().code() = HttpStatusCode::RangeNotSatisfiable_416;
                                    response.header().version() = HttpVersion::Http_Version_1_1;
                                    response.header().headerPairs().addHeaderPair("Content-Range", "bytes */" + std::to_string(file_size));
                                    co_await writer.reply(response, settings.send_timeout);
                                }
                                co_return nil();
                            }
                        } catch (const std::exception& e) {
                            HTTPS_LOG_DEBUG(
                                "[HttpsRouter] Failed to parse range: {}", e.what()
                            );
                        }
                    }
                }
            }
            
            // 确定传输模式
            std::string transfer_mode;
            
            if (settings.use_chunked_transfer) {
                transfer_mode = "chunked";
            } else {
                transfer_mode = "content-length";
            }
            
                HTTPS_LOG_DEBUG(
                    "[HttpsRouter] Sending file, size: {} bytes, mode: {}, range: {}", 
                    file_size, transfer_mode, is_range_request ? "yes" : "no"
                );
            
            // 检查连接状态
            if (conn.isClosed()) {
                HTTPS_LOG_DEBUG("[HttpsRouter] Connection closed before sending");
                co_return nil();
            }
            
            // 构建文件传输信息（用于回调）
            FileTransferInfo file_info;
            file_info.file_path = full_path.string();
            file_info.relative_path = relative_file;
            file_info.mime_type = MimeType::convertToMimeType(extension);
            file_info.file_size = file_size;
            file_info.range_start = range_start;
            file_info.range_end = range_end;
            file_info.is_range_request = is_range_request;
            if (settings.use_chunked_transfer && !is_range_request) {
                // ========== 模式1: Chunked 传输（内存占用小，但浏览器无法显示完整进度） ==========
                HttpResponseHeader header;
                header.code() = HttpStatusCode::OK_200;
                header.version() = HttpVersion::Http_Version_1_1;
                header.headerPairs().addHeaderPair("Content-Type", MimeType::convertToMimeType(extension));
                
                // 添加 Accept-Ranges 表示支持断点续传（但当前 chunked 传输不支持）
                if (settings.support_range) {
                    header.headerPairs().addHeaderPair("Accept-Ranges", "bytes");
                }
                
                // replyChunkHeader 会自动添加 Transfer-Encoding: chunked
                
                // 发送响应头
                auto header_res = co_await writer.replyChunkHeader(header, settings.send_timeout);
                if (!header_res) {
                    HTTPS_LOG_DEBUG("[HttpsRouter] Send header failed: {}", header_res.error().message());
                    conn.markClosed();
                    co_return nil();
                }
                
                // 流式发送文件内容
                std::vector<char> buffer(settings.chunk_buffer_size);
                std::ifstream file(full_path.string(), std::ios::binary);
                
                if (!file) {
                    HTTPS_LOG_DEBUG("[HttpsRouter] Failed to open file");
                    if (!conn.isClosed()) {
                        co_await conn.close();
                    }
                    co_return nil();
                }
                
                // 调用开始传输回调
                if (callback) {
                    callback(request, 0, file_size, file_info);
                }
                
                size_t total_sent = 0;
                while (file && !file.eof()) {
                    if (conn.isClosed()) {
                        HTTPS_LOG_DEBUG(
                            "[HttpsRouter] Connection closed during transfer at {}/{} bytes", 
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
                            HTTPS_LOG_DEBUG(
                                "[HttpsRouter] Send chunk failed at {}/{} bytes: {}", 
                                total_sent, file_size, chunk_res.error().message()
                            );
                            file.close();
                            conn.markClosed();
                            co_return nil();
                        }
                        
                        // 调用传输进度回调
                        if (callback) {
                            callback(request, total_sent, file_size, file_info);
                        }
                    }
                }
                
                file.close();
                HTTPS_LOG_DEBUG(
                    "[HttpsRouter] File sent successfully (chunked): {} bytes", total_sent
                );
                
            } else {
                // ========== 模式2: Content-Length 传输（浏览器显示完整进度，支持断点续传） ==========
                HttpResponse response;
                
                // Range 请求返回 206，否则返回 200
                if (is_range_request) {
                    response.header().code() = HttpStatusCode::PartialContent_206;
                } else {
                    response.header().code() = HttpStatusCode::OK_200;
                }
                
                response.header().version() = HttpVersion::Http_Version_1_1;
                
                // 计算实际发送的内容长度
                size_t content_length = range_end - range_start + 1;
                response.header().headerPairs().addHeaderPair("Content-Length", std::to_string(content_length));
                response.header().headerPairs().addHeaderPair("Content-Type", MimeType::convertToMimeType(extension));
                
                // 添加 Accept-Ranges 表示支持断点续传
                if (settings.support_range) {
                    response.header().headerPairs().addHeaderPair("Accept-Ranges", "bytes");
                }
                
                // Range 请求需要添加 Content-Range 头
                if (is_range_request) {
                    std::string content_range = "bytes " + std::to_string(range_start) + "-" + 
                                                std::to_string(range_end) + "/" + std::to_string(file_size);
                    response.header().headerPairs().addHeaderPair("Content-Range", content_range);
                    HTTPS_LOG_DEBUG(
                        "[HttpsRouter] Range response: {} bytes ({}-{}/{})",
                        content_length, range_start, range_end, file_size
                    );
                }
                
                // 调用开始传输回调
                if (callback) {
                    callback(request, 0, content_length, file_info);
                }
                
                // 读取文件（Range 请求只读取指定范围）
                std::ifstream file(full_path.string(), std::ios::binary);
                if (!file) {
                    HTTPS_LOG_DEBUG("[HttpsRouter] Failed to open file");
                    if (!conn.isClosed()) {
                        co_await conn.close();
                    }
                    co_return nil();
                }
                
                // 如果是 Range 请求，跳到起始位置
                if (is_range_request) {
                    file.seekg(range_start);
                }
                
                std::string file_content;
                file_content.resize(content_length);
                file.read(file_content.data(), content_length);
                size_t bytes_read = file.gcount();
                file.close();
                
                if (bytes_read != content_length) {
                    HTTPS_LOG_DEBUG(
                        "[HttpsRouter] Failed to read complete range: expected {}, got {}", 
                        content_length, bytes_read
                    );
                    if (!conn.isClosed()) {
                        co_await conn.close();
                    }
                    co_return nil();
                }
                
                response.setBodyStr(std::move(file_content));
                
                // 发送完整响应
                auto send_res = co_await writer.reply(response, settings.send_timeout);
                if (!send_res) {
                    HTTPS_LOG_DEBUG("[HttpsRouter] Send response failed: {}", send_res.error().message());
                    conn.markClosed();
                    co_return nil();
                }
                
                // 调用传输完成回调
                if (callback) {
                    callback(request, content_length, content_length, file_info);
                }
                
                HTTPS_LOG_DEBUG(
                    "[HttpsRouter] File sent successfully (content-length): {} bytes", content_length
                );
            }
            
        } catch (const std::filesystem::filesystem_error& e) {
            has_error = true;
            error_msg = std::string("Filesystem error: ") + e.what();
            HTTPS_LOG_ERROR("[HttpsRouter] {}", error_msg);
        } catch (const std::exception& e) {
            has_error = true;
            error_msg = std::string("Error: ") + e.what();
            HTTPS_LOG_ERROR("[HttpsRouter] {}", error_msg);
        }
        
        // 在 catch 块外处理错误，这里可以使用 co_await
        if (has_error && !conn.isClosed()) {
            HTTPS_LOG_DEBUG("[HttpsRouter] Handling error: {}", error_msg);
            auto response = HttpUtils::defaultInternalServerError();
            auto result = co_await writer.reply(response, settings.send_timeout);
            if (!result) {
                // 如果发送错误响应失败，说明连接已断开，只标记状态
                HTTPS_LOG_ERROR("[HttpsRouter] Failed to send error response");
                conn.markClosed();
                co_return nil();
            }
            // 发送错误响应成功，正常关闭连接
            co_await conn.close();
        }
        
        co_return nil();
    }
}

