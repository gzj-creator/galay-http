#include "Http2StreamHelper.h"
#include "Http2Writer.h"
#include "galay-http/protoc/http2/Http2Hpack.h"
#include "galay-http/protoc/http/HttpBase.h"
#include "galay-http/utils/Http2DebugLog.h"
#include "galay/kernel/coroutine/AsyncWaiter.hpp"
#include <fstream>
#include <algorithm>

namespace galay::http
{
    namespace fs = std::filesystem;
    
    Http2StreamHelper::Http2StreamHelper(Http2Connection& conn, uint32_t stream_id)
        : m_conn(conn), m_stream_id(stream_id)
    {
    }
    
    std::string Http2StreamHelper::getMimeType(const std::string& file_path)
    {
        std::string ext = fs::path(file_path).extension().string();
        
        if (!ext.empty() && ext[0] == '.') {
            ext = ext.substr(1);
        }
        
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        std::string mime = MimeType::convertToMimeType(ext);
        
        return mime.empty() ? "application/octet-stream" : mime;
    }
    
    Coroutine<bool> Http2StreamHelper::sendFile(const std::string& file_path,
                                                ProgressCallback progress_callback)
    {
        // 检查文件是否存在
        if (!fs::exists(file_path) || !fs::is_regular_file(file_path)) {
            HTTP2_LOG_WARN("[Http2StreamHelper] File not found: {}", file_path);
            AsyncWaiter<void, Http2Error> waiter;
            auto co = sendError(NotFound_404, "File Not Found");
            co.then([&waiter](){ waiter.notify({}); });
            waiter.appendTask(std::move(co));
            co_await waiter.wait();
            co_return false;
        }
        
        size_t file_size = fs::file_size(file_path);
        std::string filename = fs::path(file_path).filename().string();
        std::string mime_type = getMimeType(file_path);
        
        HTTP2_LOG_INFO("[Http2StreamHelper] Sending file: {} ({} bytes, {})",
                      filename, file_size, mime_type);
        
        // 构建响应头
        std::map<std::string, std::string> headers = {
            {"content-type", mime_type},
            {"content-length", std::to_string(file_size)},
            {"content-disposition", "inline; filename=\"" + filename + "\""},
            {"cache-control", "public, max-age=3600"}
        };
        
        // 发送响应头
        HpackEncoder encoder;
        std::vector<HpackHeaderField> header_fields = {{":status", "200"}};
        for (const auto& [key, value] : headers) {
            header_fields.push_back({key, value});
        }
        std::string encoded_headers = encoder.encodeHeaders(header_fields);
        
        auto writer = m_conn.getWriter(m_settings);
        auto headers_result = co_await writer.sendHeaders(m_stream_id, encoded_headers, false, true);
        if (!headers_result.has_value()) {
            HTTP2_LOG_ERROR("[Http2StreamHelper] Failed to send headers");
            m_conn.streamManager().removeStream(m_stream_id);
            co_return false;
        }
        
        // 打开文件
        std::ifstream file(file_path, std::ios::binary);
        if (!file) {
            HTTP2_LOG_ERROR("[Http2StreamHelper] Failed to open file: {}", file_path);
            m_conn.streamManager().removeStream(m_stream_id);
            co_return false;
        }
        
        // 分块发送文件
        const size_t CHUNK_SIZE = m_settings.max_frame_size > 0 ? m_settings.max_frame_size : 16384;
        std::vector<char> buffer(CHUNK_SIZE);
        size_t total_sent = 0;
        
        while (file) {
            file.read(buffer.data(), CHUNK_SIZE);
            std::streamsize bytes_read = file.gcount();
            
            if (bytes_read > 0) {
                bool is_last = file.eof();
                std::string chunk(buffer.data(), bytes_read);
                
                auto result = co_await writer.sendData(m_stream_id, chunk, is_last);
                if (!result.has_value()) {
                    HTTP2_LOG_ERROR("[Http2StreamHelper] Failed to send data chunk");
                    file.close();
                    m_conn.streamManager().removeStream(m_stream_id);
                    co_return false;
                }
                
                total_sent += bytes_read;
                
                // 调用进度回调
                if (progress_callback) {
                    progress_callback(total_sent, file_size);
                }
            }
        }
        
        file.close();
        m_conn.streamManager().removeStream(m_stream_id);
        
        HTTP2_LOG_INFO("[Http2StreamHelper] File sent successfully: {}", filename);
        co_return true;
    }
    
    Coroutine<bool> Http2StreamHelper::sendText(HttpStatusCode status_code,
                                                const std::string& body,
                                                const std::string& content_type)
    {
        std::map<std::string, std::string> headers = {
            {"content-type", content_type},
            {"content-length", std::to_string(body.length())}
        };
        
        AsyncWaiter<void, Http2Error> waiter;
        auto co = sendResponse(status_code, headers, body);
        co.then([&waiter](){ waiter.notify({}); });
        waiter.appendTask(std::move(co));
        auto result = co_await waiter.wait();
        co_return result.has_value();
    }
    
    Coroutine<bool> Http2StreamHelper::sendJson(HttpStatusCode status_code, const std::string& json_body)
    {
        AsyncWaiter<void, Http2Error> waiter;
        auto co = sendText(status_code, json_body, "application/json");
        co.then([&waiter](){ waiter.notify({}); });
        waiter.appendTask(std::move(co));
        auto result = co_await waiter.wait();
        co_return result.has_value();
    }
    
    Coroutine<bool> Http2StreamHelper::sendHtml(HttpStatusCode status_code, const std::string& html_body)
    {
        AsyncWaiter<void, Http2Error> waiter;
        auto co = sendText(status_code, html_body, "text/html; charset=utf-8");
        co.then([&waiter](){ waiter.notify({}); });
        waiter.appendTask(std::move(co));
        auto result = co_await waiter.wait();
        co_return result.has_value();
    }
    
    Coroutine<bool> Http2StreamHelper::sendError(HttpStatusCode status_code, const std::string& message)
    {
        std::string body = message.empty() ? httpStatusCodeToString(status_code) : message;
        AsyncWaiter<void, Http2Error> waiter;
        auto co = sendText(status_code, body, "text/plain");
        co.then([&waiter](){ waiter.notify({}); });
        waiter.appendTask(std::move(co));
        auto result = co_await waiter.wait();
        co_return result.has_value();
    }
    
    Coroutine<bool> Http2StreamHelper::sendResponse(HttpStatusCode status_code,
                                                    const std::map<std::string, std::string>& headers,
                                                    const std::string& body)
    {
        // 构建响应头
        HpackEncoder encoder;
        std::vector<HpackHeaderField> header_fields = {
            {":status", std::to_string(static_cast<int>(status_code))}
        };
        
        for (const auto& [key, value] : headers) {
            header_fields.push_back({key, value});
        }
        
        std::string encoded_headers = encoder.encodeHeaders(header_fields);
        
        auto writer = m_conn.getWriter(m_settings);
        
        // 发送响应头
        auto headers_result = co_await writer.sendHeaders(m_stream_id, encoded_headers, false, true);
        if (!headers_result.has_value()) {
            HTTP2_LOG_ERROR("[Http2StreamHelper] Failed to send headers");
            m_conn.streamManager().removeStream(m_stream_id);
            co_return false;
        }
        
        // 发送响应体
        auto data_result = co_await writer.sendData(m_stream_id, body, true);
        if (!data_result.has_value()) {
            HTTP2_LOG_ERROR("[Http2StreamHelper] Failed to send data");
            m_conn.streamManager().removeStream(m_stream_id);
            co_return false;
        }
        
        m_conn.streamManager().removeStream(m_stream_id);
        co_return true;
    }
    
    Coroutine<bool> Http2StreamHelper::sendHeaders(HttpStatusCode status_code,
                                                   const std::map<std::string, std::string>& headers)
    {
        HpackEncoder encoder;
        std::vector<HpackHeaderField> header_fields = {
            {":status", std::to_string(static_cast<int>(status_code))}
        };
        
        for (const auto& [key, value] : headers) {
            header_fields.push_back({key, value});
        }
        
        std::string encoded_headers = encoder.encodeHeaders(header_fields);
        
        auto writer = m_conn.getWriter(m_settings);
        auto result = co_await writer.sendHeaders(m_stream_id, encoded_headers, false, true);
        
        co_return result.has_value();
    }
    
    Coroutine<bool> Http2StreamHelper::sendData(const std::string& data, bool end_stream)
    {
        auto writer = m_conn.getWriter(m_settings);
        auto result = co_await writer.sendData(m_stream_id, data, end_stream);
        
        if (end_stream) {
            m_conn.streamManager().removeStream(m_stream_id);
        }
        
        co_return result.has_value();
    }
    
    // ==================== Http2StaticFileServer ====================
    
    Coroutine<bool> Http2StaticFileServer::serve(Http2Connection& conn,
                                                 uint32_t stream_id,
                                                 const std::string& url_prefix,
                                                 const std::string& local_dir,
                                                 const std::string& request_path,
                                                 ProgressCallback progress_callback)
    {
        Http2StreamHelper helper(conn, stream_id);
        
        // 检查路径是否匹配前缀
        if (request_path.find(url_prefix) != 0) {
            AsyncWaiter<void, Http2Error> waiter;
            auto co = helper.sendError(NotFound_404);
            co.then([&waiter](){ waiter.notify({}); });
            waiter.appendTask(std::move(co));
            co_await waiter.wait();
            co_return false;
        }
        
        // 提取相对路径
        std::string relative_path = request_path.substr(url_prefix.length());
        if (relative_path.empty() || relative_path[0] != '/') {
            relative_path = "/" + relative_path;
        }
        
        // 构建文件路径
        fs::path file_path = fs::path(local_dir) / relative_path.substr(1);
        
        // 安全检查：防止目录遍历攻击
        std::string canonical_base;
        std::string canonical_file;
        bool file_not_found = false;
        
        try {
            canonical_base = fs::canonical(local_dir).string();
            canonical_file = fs::canonical(file_path).string();
        } catch (const std::exception& e) {
            file_not_found = true;
        }
        
        if (file_not_found) {
            HTTP2_LOG_WARN("[Http2StaticFileServer] File not found: {}", file_path.string());
            AsyncWaiter<void, Http2Error> waiter;
            auto co = helper.sendError(NotFound_404);
            co.then([&waiter](){ waiter.notify({}); });
            waiter.appendTask(std::move(co));
            co_await waiter.wait();
            co_return false;
        }
        
        if (canonical_file.find(canonical_base) != 0) {
            HTTP2_LOG_WARN("[Http2StaticFileServer] Security: Path traversal attempt: {}", request_path);
            AsyncWaiter<void, Http2Error> waiter;
            auto co = helper.sendError(Forbidden_403);
            co.then([&waiter](){ waiter.notify({}); });
            waiter.appendTask(std::move(co));
            co_await waiter.wait();
            co_return false;
        }
        
        // 发送文件
        AsyncWaiter<void, Http2Error> waiter;
        auto co = helper.sendFile(canonical_file, 
            [&progress_callback, canonical_file](size_t sent, size_t total) {
                if (progress_callback) {
                    progress_callback(canonical_file, sent, total);
                }
            }
        );
        co.then([&waiter](){ waiter.notify({}); });
        waiter.appendTask(std::move(co));
        auto result = co_await waiter.wait();
        
        co_return result.has_value();
    }
}

