#include "HttpRouter.h"
#include "HttpConnection.h"
#include "galay-http/protoc/http/HttpBase.h"
#include "galay-http/protoc/http/HttpError.h"
#include "galay-http/utils/HttpLogger.h"
#include "galay-http/utils/HttpDebugLog.h"
#include "galay-http/utils/HttpUtils.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <algorithm>  // for std::min
#include <chrono>     // for time measurement
#include <galay/utils/System.h>
#include <string>
#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#include <cstring>  // for strerror
#endif

namespace galay::http
{
    void HttpRouter::mount(const std::string& prefix, const std::string& path, HttpSettings setting)
    {
        HTTP_LOG_DEBUG("[HttpRouter] Mount {} -> {}", prefix, path);
        
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
        HTTP_LOG_DEBUG("[HttpRouter] Route {} {}", httpMethodToString(header.method()), header.uri());
        
        // 尝试模板匹配（带参数或通配符）
        HttpParams params;
        //middleware
        // 首先尝试精确匹配（性能更好）
        if(auto it = m_routes[static_cast<int>(header.method())].find(header.uri()); it != m_routes[static_cast<int>(header.method())].end()) {
            HTTP_LOG_DEBUG("[HttpRouter] Exact match found");
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
                HTTP_LOG_DEBUG("[HttpRouter] Template match found: {}", template_uri);
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
        HTTP_LOG_DEBUG("[HttpRouter] No route found");
        return {std::unexpected(HttpError(HttpErrorCode::kHttpError_NotFound))};
    }

    Coroutine<nil> HttpRouter::staticFileRoute(std::string path, HttpSettings settings, HttpRequest& request, HttpConnection& conn, HttpParams params)
    {
        // 检查连接是否已关闭
        if (conn.isClosed()) {
            HTTP_LOG_DEBUG("[HttpRouter] Connection already closed");
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
            
            HTTP_LOG_DEBUG("[HttpRouter] Serve file: {}", relative_file);
            
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
                HTTP_LOG_DEBUG("[HttpRouter] Path traversal attempt blocked");
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
                HTTP_LOG_DEBUG(
                    "[HttpRouter] Range request: {}", range_header
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
                                HTTP_LOG_DEBUG(
                                    "[HttpRouter] Valid range: {}-{}/{}", 
                                    range_start, range_end, file_size
                                );
                            } else {
                                HTTP_LOG_DEBUG(
                                    "[HttpRouter] Invalid range: {}-{}/{}", 
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
                            HTTP_LOG_DEBUG(
                                "[HttpRouter] Failed to parse range: {}", e.what()
                            );
                        }
                    }
                }
            }
            
            // 确定传输模式
            std::string transfer_mode;
#ifdef __linux__
            if (settings.use_sendfile) {
                transfer_mode = "sendfile (zero-copy)";
            } else
#endif
            if (settings.use_chunked_transfer) {
                transfer_mode = "chunked";
            } else {
                transfer_mode = "content-length";
            }
            
                HTTP_LOG_DEBUG(
                    "[HttpRouter] Sending file, size: {} bytes, mode: {}, range: {}", 
                    file_size, transfer_mode, is_range_request ? "yes" : "no"
                );
            
            // 检查连接状态
            if (conn.isClosed()) {
                HTTP_LOG_DEBUG("[HttpRouter] Connection closed before sending");
                co_return nil();
            }
            
#ifdef __linux__
            if (settings.use_sendfile) {
                // ========== 模式3: Sendfile 零拷贝传输（仅 Linux，性能最佳，支持断点续传） ==========
                HttpResponseHeader header;
                
                // Range 请求返回 206，否则返回 200
                if (is_range_request) {
                    header.code() = HttpStatusCode::PartialContent_206;
                } else {
                    header.code() = HttpStatusCode::OK_200;
                }
                
                header.version() = HttpVersion::Http_Version_1_1;
                
                // 计算实际发送的内容长度
                size_t content_length = range_end - range_start + 1;
                header.headerPairs().addHeaderPair("Content-Length", std::to_string(content_length));
                header.headerPairs().addHeaderPair("Content-Type", MimeType::convertToMimeType(extension));
                
                // 添加 Accept-Ranges 表示支持断点续传
                if (settings.support_range) {
                    header.headerPairs().addHeaderPair("Accept-Ranges", "bytes");
                }
                
                // Range 请求需要添加 Content-Range 头
                if (is_range_request) {
                    std::string content_range = "bytes " + std::to_string(range_start) + "-" + 
                                                std::to_string(range_end) + "/" + std::to_string(file_size);
                    header.headerPairs().addHeaderPair("Content-Range", content_range);
                    HTTP_LOG_DEBUG(
                        "[HttpRouter] Range response: {} bytes ({}-{}/{})",
                        content_length, range_start, range_end, file_size
                    );
                }
                
                // 发送响应头
                HttpResponse response;
                response.setHeader(header);
                auto header_res = co_await writer.reply(response, settings.send_timeout);
                if (!header_res) {
                    HTTP_LOG_DEBUG("[HttpRouter] Send header failed: {}", header_res.error().message());
                    conn.markClosed();
                    co_return nil();
                }
                
                // 打开文件获取文件描述符
                // 注意：sendfile() 的文件 fd 必须是阻塞模式！
                // O_NONBLOCK 会导致 sendfile() 立即返回 EAGAIN
                // 只有 socket fd 需要 O_NONBLOCK，文件 fd 必须是阻塞的
                int file_fd = open(full_path.string().c_str(), O_RDONLY);
                if (file_fd < 0) {
                    HTTP_LOG_DEBUG("[HttpRouter] Failed to open file for sendfile: {}", strerror(errno));
                    if (!conn.isClosed()) {
                        co_await conn.close();
                    }
                    co_return nil();
                }
                
                // 应用层循环发送文件，记录详细的性能数据
                // Range 请求从 range_start 开始
                off_t offset = range_start;
                size_t total_sent = 0;
                size_t bytes_to_send = content_length;  // 需要发送的总字节数
                
                auto start_time = std::chrono::steady_clock::now();
                int iteration_count = 0;
                
                HTTP_LOG_INFO(
                    "[HttpRouter] ========== Sendfile Start: {} bytes ==========",
                    bytes_to_send
                );
                
                while (total_sent < bytes_to_send) {
                    iteration_count++;
                    auto iter_start = std::chrono::steady_clock::now();
                    
                    // 检查连接状态
                    if (conn.isClosed()) {
                        HTTP_LOG_WARN(
                            "[HttpRouter] Connection closed during sendfile at {}/{} bytes (iteration: {})",
                            total_sent, bytes_to_send, iteration_count
                        );
                        close(file_fd);
                        co_return nil();
                    }
                    
                    // 计算剩余字节数
                    size_t remaining = bytes_to_send - total_sent;
                    size_t chunk_size = std::min(settings.sendfile_chunk_size, remaining);
                    
                    HTTP_LOG_INFO(
                        "[HttpRouter] [Iter {}] Before sendfile: sent={}/{} ({:.1f}%), offset={}, chunk_size={}",
                        iteration_count, total_sent, bytes_to_send, 
                        (total_sent * 100.0 / bytes_to_send), offset, chunk_size
                    );
                    
                    // 发送数据
                    auto sendfile_res = co_await writer.sendfile(file_fd, offset, chunk_size);
                    auto iter_end = std::chrono::steady_clock::now();
                    auto iter_duration = std::chrono::duration_cast<std::chrono::milliseconds>(iter_end - iter_start).count();
                    
                    if (!sendfile_res) {
                        HTTP_LOG_ERROR(
                            "[HttpRouter] [Iter {}] Sendfile failed at {}/{} bytes: {}",
                            iteration_count, total_sent, bytes_to_send, sendfile_res.error().message()
                        );
                        close(file_fd);
                        conn.markClosed();
                        co_return nil();
                    }
                    
                    long bytes_sent = sendfile_res.value();
                    total_sent += bytes_sent;
                    offset += bytes_sent;
                    
                    // 计算速度
                    double speed_kbps = (bytes_sent / 1024.0) / (iter_duration / 1000.0);
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(iter_end - start_time).count();
                    double avg_speed_kbps = (total_sent / 1024.0) / (elapsed / 1000.0);
                    
                    HTTP_LOG_INFO(
                        "[HttpRouter] [Iter {}] Sent: {} bytes in {} ms ({:.1f} KB/s), total={}/{} ({:.1f}%), avg_speed={:.1f} KB/s",
                        iteration_count, bytes_sent, iter_duration, speed_kbps,
                        total_sent, bytes_to_send, (total_sent * 100.0 / bytes_to_send), avg_speed_kbps
                    );
                    
                    // 如果单次发送字节数异常小，警告
                    if (bytes_sent < 8192 && remaining >= 8192) {
                        HTTP_LOG_WARN(
                            "[HttpRouter] [Iter {}] WARNING: Only sent {} bytes (expected more)",
                            iteration_count, bytes_sent
                        );
                    }
                }
                
                // 关闭文件描述符
                close(file_fd);
                
                auto end_time = std::chrono::steady_clock::now();
                auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
                double total_speed_kbps = (total_sent / 1024.0) / (total_duration / 1000.0);
                
                HTTP_LOG_INFO(
                    "[HttpRouter] ========== Sendfile Complete: {} bytes in {} ms ({:.1f} KB/s, {} iterations) ==========",
                    total_sent, total_duration, total_speed_kbps, iteration_count
                );
                
            } else
#endif
            if (settings.use_chunked_transfer && !is_range_request) {
                // ========== 模式1: Chunked 传输（内存占用小，但浏览器无法显示完整进度） ==========
                // 注意：Chunked 模式不支持 Range 请求，Range 请求会自动降级到 Content-Length 模式
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
                    HTTP_LOG_DEBUG("[HttpRouter] Send header failed: {}", header_res.error().message());
                    conn.markClosed();
                    co_return nil();
                }
                
                // 流式发送文件内容
                std::vector<char> buffer(settings.chunk_buffer_size);
                std::ifstream file(full_path.string(), std::ios::binary);
                
                if (!file) {
                    HTTP_LOG_DEBUG("[HttpRouter] Failed to open file");
                    if (!conn.isClosed()) {
                        co_await conn.close();
                    }
                    co_return nil();
                }
                
                size_t total_sent = 0;
                while (file && !file.eof()) {
                    if (conn.isClosed()) {
                        HTTP_LOG_DEBUG(
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
                            HTTP_LOG_DEBUG(
                                "[HttpRouter] Send chunk failed at {}/{} bytes: {}", 
                                total_sent, file_size, chunk_res.error().message()
                            );
                            file.close();
                            conn.markClosed();
                            co_return nil();
                        }
                    }
                }
                
                file.close();
                HTTP_LOG_DEBUG(
                    "[HttpRouter] File sent successfully (chunked): {} bytes", total_sent
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
                    HTTP_LOG_DEBUG(
                        "[HttpRouter] Range response: {} bytes ({}-{}/{})",
                        content_length, range_start, range_end, file_size
                    );
                }
                
                // 读取文件（Range 请求只读取指定范围）
                std::ifstream file(full_path.string(), std::ios::binary);
                if (!file) {
                    HTTP_LOG_DEBUG("[HttpRouter] Failed to open file");
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
                    HTTP_LOG_DEBUG(
                        "[HttpRouter] Failed to read complete range: expected {}, got {}", 
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
                    HTTP_LOG_DEBUG("[HttpRouter] Send response failed: {}", send_res.error().message());
                    conn.markClosed();
                    co_return nil();
                }
                
                HTTP_LOG_DEBUG(
                    "[HttpRouter] File sent successfully (content-length): {} bytes", file_size
                );
            }
            
        } catch (const std::filesystem::filesystem_error& e) {
            has_error = true;
            error_msg = std::string("Filesystem error: ") + e.what();
            HTTP_LOG_ERROR("[HttpRouter] {}", error_msg);
        } catch (const std::exception& e) {
            has_error = true;
            error_msg = std::string("Error: ") + e.what();
            HTTP_LOG_ERROR("[HttpRouter] {}", error_msg);
        }
        
        // 在 catch 块外处理错误，这里可以使用 co_await
        if (has_error && !conn.isClosed()) {
            HTTP_LOG_DEBUG("[HttpRouter] Handling error: {}", error_msg);
            auto response = HttpUtils::defaultInternalServerError();
            auto result = co_await writer.reply(response, settings.send_timeout);
            if (!result) {
                // 如果发送错误响应失败，说明连接已断开，只标记状态
                HTTP_LOG_ERROR("[HttpRouter] Failed to send error response");
                conn.markClosed();
                co_return nil();
            }
            // 发送错误响应成功，正常关闭连接
            co_await conn.close();
        }
        
        co_return nil();
    }
}
