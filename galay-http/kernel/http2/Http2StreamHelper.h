#ifndef GALAY_HTTP2_STREAM_HELPER_H
#define GALAY_HTTP2_STREAM_HELPER_H

#include "Http2Connection.h"
#include "Http2Params.hpp"
#include "galay-http/protoc/http/HttpBase.h"
#include <string>
#include <functional>

namespace galay::http
{
    using enum HttpStatusCode;
    /**
     * @brief HTTP/2 流辅助类
     * 
     * 高度封装的 HTTP/2 操作接口，简化用户对帧的操作
     * 用户不需要直接操作 HPACK、帧等底层细节
     * 
     * 使用示例：
     * @code
     * Http2StreamHelper helper(conn, stream_id);
     * 
     * // 发送文件（自动分片、流控）
     * co_await helper.sendFile("/path/to/file.jpg");
     * 
     * // 发送 JSON 响应
     * co_await helper.sendJson(200, {{"message", "success"}});
     * 
     * // 发送错误
     * co_await helper.sendError(404, "Not Found");
     * @endcode
     */
    class Http2StreamHelper
    {
    public:
        /**
         * @brief 文件传输进度回调
         * @param bytes_sent 已发送字节数
         * @param total_bytes 文件总大小
         */
        using ProgressCallback = std::function<void(size_t bytes_sent, size_t total_bytes)>;
        
        /**
         * @brief 构造函数
         * @param conn HTTP/2 连接对象
         * @param stream_id 流 ID
         */
        Http2StreamHelper(Http2Connection& conn, uint32_t stream_id);
        
        /**
         * @brief 发送文件（自动处理分片、流控、MIME类型）
         * 
         * @param file_path 文件路径
         * @param progress_callback 进度回调（可选）
         * @return Coroutine<bool> 成功返回 true，失败返回 false
         * 
         * @example
         * Http2StreamHelper helper(conn, stream_id);
         * bool success = co_await helper.sendFile("/data/image.jpg", 
         *     [](size_t sent, size_t total) {
         *         std::cout << "Progress: " << (sent * 100.0 / total) << "%" << std::endl;
         *     }
         * );
         */
        Coroutine<bool> sendFile(const std::string& file_path,
                                 ProgressCallback progress_callback = nullptr);
        
        /**
         * @brief 发送文本响应
         * 
         * @param status_code HTTP 状态码
         * @param body 响应体
         * @param content_type Content-Type（默认 text/plain）
         * @return Coroutine<bool> 成功返回 true
         * 
         * @example
         * co_await helper.sendText(OK_200, "Hello World");
         */
        Coroutine<bool> sendText(HttpStatusCode status_code,
                                 const std::string& body,
                                 const std::string& content_type = "text/plain");
        
        /**
         * @brief 发送 JSON 响应
         * 
         * @param status_code HTTP 状态码
         * @param json_body JSON 字符串
         * @return Coroutine<bool> 成功返回 true
         * 
         * @example
         * co_await helper.sendJson(OK_200, R"({"status": "ok"})");
         */
        Coroutine<bool> sendJson(HttpStatusCode status_code, const std::string& json_body);
        
        /**
         * @brief 发送 HTML 响应
         * 
         * @param status_code HTTP 状态码
         * @param html_body HTML 内容
         * @return Coroutine<bool> 成功返回 true
         * 
         * @example
         * co_await helper.sendHtml(OK_200, "<h1>Hello</h1>");
         */
        Coroutine<bool> sendHtml(HttpStatusCode status_code, const std::string& html_body);
        
        /**
         * @brief 发送错误响应
         * 
         * @param status_code HTTP 状态码
         * @param message 错误消息
         * @return Coroutine<bool> 成功返回 true
         * 
         * @example
         * co_await helper.sendError(NotFound_404, "Not Found");
         * co_await helper.sendError(InternalServerError_500, "Internal Server Error");
         */
        Coroutine<bool> sendError(HttpStatusCode status_code, const std::string& message = "");
        
        /**
         * @brief 发送自定义响应
         * 
         * @param status_code HTTP 状态码
         * @param headers 自定义头部（不包括 :status）
         * @param body 响应体
         * @return Coroutine<bool> 成功返回 true
         * 
         * @example
         * std::map<std::string, std::string> headers = {
         *     {"content-type", "application/pdf"},
         *     {"content-disposition", "attachment; filename=doc.pdf"}
         * };
         * co_await helper.sendResponse(OK_200, headers, pdf_data);
         */
        Coroutine<bool> sendResponse(HttpStatusCode status_code,
                                     const std::map<std::string, std::string>& headers,
                                     const std::string& body);
        
        /**
         * @brief 开始发送响应（只发送头部，用于流式传输）
         * 
         * @param status_code HTTP 状态码
         * @param headers 响应头部
         * @return Coroutine<bool> 成功返回 true
         */
        Coroutine<bool> sendHeaders(HttpStatusCode status_code,
                                    const std::map<std::string, std::string>& headers);
        
        /**
         * @brief 发送数据块（用于流式传输）
         * 
         * @param data 数据
         * @param end_stream 是否结束流
         * @return Coroutine<bool> 成功返回 true
         */
        Coroutine<bool> sendData(const std::string& data, bool end_stream = false);
        
        /**
         * @brief 获取流 ID
         */
        uint32_t streamId() const { return m_stream_id; }
        
    private:
        Http2Connection& m_conn;
        uint32_t m_stream_id;
        Http2Settings m_settings;
        
        // 辅助方法：获取 MIME 类型
        std::string getMimeType(const std::string& file_path);
    };
    
    /**
     * @brief 静态文件服务辅助类
     * 
     * 提供静态文件服务的便捷接口
     * 
     * @example
     * Http2StaticFileServer::serve(conn, stream_id, "/static", "./public", request_path);
     */
    class Http2StaticFileServer
    {
    public:
        using ProgressCallback = std::function<void(
            const std::string& file_path,
            size_t bytes_sent,
            size_t total_bytes
        )>;
        
        /**
         * @brief 提供静态文件服务
         * 
         * @param conn HTTP/2 连接
         * @param stream_id 流 ID
         * @param url_prefix URL 前缀（如 "/static"）
         * @param local_dir 本地目录（如 "./public"）
         * @param request_path 请求路径（如 "/static/image.jpg"）
         * @param progress_callback 进度回调（可选）
         * @return Coroutine<bool> 成功返回 true
         * 
         * @example
         * bool served = co_await Http2StaticFileServer::serve(
         *     conn, stream_id, "/static", "./public", "/static/image.jpg"
         * );
         */
        static Coroutine<bool> serve(Http2Connection& conn,
                                     uint32_t stream_id,
                                     const std::string& url_prefix,
                                     const std::string& local_dir,
                                     const std::string& request_path,
                                     ProgressCallback progress_callback = nullptr);
    };
}

#endif // GALAY_HTTP2_STREAM_HELPER_H

