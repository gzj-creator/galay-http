#include "Http2Router.h"
#include "Http2Writer.h"
#include "galay-http/protoc/http2/Http2Hpack.h"
#include "galay-http/protoc/http/HttpBase.h"
#include "galay-http/utils/Http2DebugLog.h"
#include <fstream>
#include <algorithm>

namespace galay::http
{    
    // 获取 MIME 类型
    static std::string getMimeType(const std::string& filename) {
        std::string ext = std::filesystem::path(filename).extension().string();
        
        if (!ext.empty() && ext[0] == '.') {
            ext = ext.substr(1);
        }
        
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        std::string mime = MimeType::convertToMimeType(ext);
        
        return mime.empty() ? "application/octet-stream" : mime;
    }
    
    void Http2Router::mount(const std::string& url_prefix,
                           const std::string& dir_path,
                           FileTransferProgressCallback callback,
                           Http2Settings settings)
    {
        if (!std::filesystem::exists(dir_path)) {
            throw std::runtime_error("Directory not found: " + dir_path);
        }
        
        std::string normalized_prefix = url_prefix;
        if (!normalized_prefix.empty() && normalized_prefix.back() == '/') {
            normalized_prefix.pop_back();
        }
        
        m_mounts[normalized_prefix] = {
            std::filesystem::absolute(dir_path).string(),
            callback,
            settings
        };
        
        HTTP2_LOG_INFO("[Http2Router] Mounted {} -> {}", url_prefix, dir_path);
    }
    
    void Http2Router::addRoute(const std::string& pattern, RouteHandler handler)
    {
        m_routes.push_back({pattern, handler});
        HTTP2_LOG_INFO("[Http2Router] Added route: {}", pattern);
    }
    
    Coroutine<nil> Http2Router::route(Http2Connection& conn,
                                     uint32_t stream_id,
                                     const std::string& method,
                                     const std::string& path,
                                     bool& handled)
    {
        handled = false;
        
        // 1. 尝试自定义路由
        for (const auto& route : m_routes) {
            if (path == route.pattern) {
                route.handler(conn, stream_id, method, path, handled).result();
                if (handled) {
                    co_return nil();
                }
            }
        }
        
        // 2. 尝试静态文件挂载点
        for (const auto& [prefix, mount_point] : m_mounts) {
            if (path.find(prefix) == 0) {
                handleStaticFile(conn, stream_id, method, path, mount_point, handled).result();
                co_return nil();
            }
        }
        
        co_return nil();
    }
    
    Coroutine<nil> Http2Router::handleStaticFile(Http2Connection& conn,
                                                uint32_t stream_id,
                                                const std::string& method,
                                                const std::string& path,
                                                const MountPoint& mount_point,
                                                bool& handled)
    {
        handled = true;
        
        // 只处理 GET 请求
        if (method != "GET") {
            sendError(conn, stream_id, 405, "Method Not Allowed").result();
            co_return nil();
        }
        
        // 提取相对路径
        std::string relative_path;
        for (const auto& [prefix, _] : m_mounts) {
            if (path.find(prefix) == 0) {
                relative_path = path.substr(prefix.length());
                if (relative_path.empty() || relative_path[0] != '/') {
                    relative_path = "/" + relative_path;
                }
                break;
            }
        }
        
        // 构建文件路径
        std::filesystem::path file_path = std::filesystem::path(mount_point.directory) / relative_path.substr(1);
        
        // 安全检查：防止目录遍历攻击
        std::string canonical_base;
        std::string canonical_file;
        
        try {
            canonical_base = std::filesystem::canonical(mount_point.directory).string();
            canonical_file = std::filesystem::canonical(file_path).string();
        } catch (const std::exception& e) {
            HTTP2_LOG_WARN("[Http2Router] File not found: {}", file_path.string());
            sendError(conn, stream_id, 404, "Not Found").result();
            co_return nil();
        }
        
        if (canonical_file.find(canonical_base) != 0) {
            HTTP2_LOG_WARN("[Http2Router] Security: Path traversal attempt: {}", path);
            sendError(conn, stream_id, 403, "Forbidden").result();
            co_return nil();
        }
        
        // 检查文件是否存在且为常规文件
        if (!std::filesystem::exists(file_path) || !std::filesystem::is_regular_file(file_path)) {
            HTTP2_LOG_WARN("[Http2Router] Not a regular file: {}", file_path.string());
            sendError(conn, stream_id, 404, "Not Found").result();
            co_return nil();
        }
        
        // 发送文件
        sendFile(conn, stream_id, file_path.string(), mount_point.callback, mount_point.settings).result();
        co_return nil();
    }
    
    Coroutine<nil> Http2Router::sendFile(Http2Connection& conn,
                                        uint32_t stream_id,
                                        const std::string& file_path,
                                        const FileTransferProgressCallback& callback,
                                        const Http2Settings& settings)
    {
        size_t file_size = std::filesystem::file_size(file_path);
        std::string filename = std::filesystem::path(file_path).filename().string();
        std::string mime_type = getMimeType(filename);
        
        HTTP2_LOG_INFO("[Http2Router] Serving: {} ({} bytes, {})", filename, file_size, mime_type);
        
        // 发送响应头
        HpackEncoder encoder;
        std::vector<HpackHeaderField> response_headers = {
            {":status", "200"},
            {"content-type", mime_type},
            {"content-length", std::to_string(file_size)},
            {"content-disposition", "inline; filename=\"" + filename + "\""},
            {"server", "galay-http2/1.0"},
            {"cache-control", "public, max-age=3600"},
            {"access-control-allow-origin", "*"}
        };
        std::string encoded_headers = encoder.encodeHeaders(response_headers);
        
        auto writer = conn.getWriter(settings);
        
        auto headers_result = co_await writer.sendHeaders(stream_id, encoded_headers, false, true);
        if (!headers_result.has_value()) {
            HTTP2_LOG_ERROR("[Http2Router] Failed to send headers");
            conn.streamManager().removeStream(stream_id);
            co_return nil();
        }
        
        // 打开文件
        std::ifstream file(file_path, std::ios::binary);
        if (!file) {
            HTTP2_LOG_ERROR("[Http2Router] Failed to open file: {}", file_path);
            conn.streamManager().removeStream(stream_id);
            co_return nil();
        }
        
        // 分块读取和发送文件
        const size_t CHUNK_SIZE = settings.max_frame_size > 0 ? settings.max_frame_size : 16384;
        std::vector<char> buffer(CHUNK_SIZE);
        size_t total_sent = 0;
        bool send_error = false;
        
        while (file && !send_error) {
            file.read(buffer.data(), CHUNK_SIZE);
            std::streamsize bytes_read = file.gcount();
            
            if (bytes_read > 0) {
                bool is_last = file.eof();
                std::string chunk(buffer.data(), bytes_read);
                
                auto result = co_await writer.sendData(stream_id, chunk, is_last);
                if (!result.has_value()) {
                    HTTP2_LOG_ERROR("[Http2Router] Failed to send data chunk");
                    send_error = true;
                    break;
                }
                
                total_sent += bytes_read;
                
                // 调用进度回调
                if (callback) {
                    callback(stream_id, file_path, total_sent, file_size);
                }
            }
        }
        
        file.close();
        
        if (!send_error) {
            HTTP2_LOG_INFO("[Http2Router] Complete: {}", filename);
        }
        
        conn.streamManager().removeStream(stream_id);
        co_return nil();
    }
    
    Coroutine<nil> Http2Router::sendError(Http2Connection& conn,
                                         uint32_t stream_id,
                                         int status_code,
                                         const std::string& message)
    {
        HpackEncoder encoder;
        std::vector<HpackHeaderField> headers = {
            {":status", std::to_string(status_code)},
            {"content-type", "text/plain"},
            {"content-length", std::to_string(message.length())}
        };
        std::string encoded = encoder.encodeHeaders(headers);
        
        auto writer = conn.getWriter({});
        co_await writer.sendHeaders(stream_id, encoded, false, true);
        co_await writer.sendData(stream_id, message, true);
        conn.streamManager().removeStream(stream_id);
        
        co_return nil();
    }
}

