#ifndef GALAY_HTTP2_ROUTER_H
#define GALAY_HTTP2_ROUTER_H

#include "Http2Connection.h"
#include "galay-http/kernel/http/HttpParams.hpp"
#include <functional>
#include <map>
#include <string>
#include <filesystem>

namespace galay::http
{
    /**
     * @brief HTTP/2 路由器
     * 
     * 提供类似 HTTP/1.1 的路由功能，支持：
     * - 静态文件服务（mount）
     * - 自定义路由处理
     * - 文件传输进度回调
     * 
     * 使用示例：
     * @code
     * Http2Router router;
     * 
     * // 挂载静态文件目录（带进度回调）
     * router.mount("/static", "./public", [](uint32_t stream_id, const std::string& path, 
     *                                         size_t sent, size_t total) {
     *     std::cout << "Progress: " << (sent * 100.0 / total) << "%" << std::endl;
     * });
     * 
     * // 在 onHeaders 回调中使用
     * auto handled = co_await router.route(conn, stream_id, method, path);
     * if (!handled) {
     *     // 处理 404 等
     * }
     * @endcode
     */
    class Http2Router
    {
    public:
        using ptr = std::shared_ptr<Http2Router>;
        
        /**
         * @brief HTTP/2 文件传输进度回调类型
         * 
         * @param stream_id HTTP/2 流 ID
         * @param file_path 文件路径
         * @param bytes_sent 已发送字节数
         * @param total_bytes 文件总大小
         */
        using FileTransferProgressCallback = std::function<void(
            uint32_t stream_id,
            const std::string& file_path,
            size_t bytes_sent,
            size_t total_bytes
        )>;
        
        /**
         * @brief 自定义路由处理函数类型
         * 
         * @param conn HTTP/2 连接对象
         * @param stream_id 流 ID
         * @param method HTTP 方法
         * @param path 请求路径
         * @return true 表示已处理，false 表示未匹配
         */
        using RouteHandler = std::function<Coroutine<nil>(
            Http2Connection& conn,
            uint32_t stream_id,
            const std::string& method,
            const std::string& path,
            bool& handled
        )>;
        
        Http2Router() = default;
        ~Http2Router() = default;
        
        /**
         * @brief 挂载静态文件目录
         * 
         * @param url_prefix URL 前缀，例如 "/static"
         * @param dir_path 本地目录路径
         * @param callback 文件传输进度回调（可选）
         * @param settings HTTP/2 传输设置（可选）
         * 
         * @throws std::runtime_error 如果目录不存在
         * 
         * @example
         * // 基本用法
         * router.mount("/static", "./public");
         * 
         * // 带进度回调
         * router.mount("/files", "./uploads", 
         *     [](uint32_t stream_id, const std::string& path, size_t sent, size_t total) {
         *         std::cout << path << ": " << (sent * 100.0 / total) << "%" << std::endl;
         *     }
         * );
         */
        void mount(const std::string& url_prefix, 
                   const std::string& dir_path,
                   FileTransferProgressCallback callback = nullptr,
                   Http2Settings settings = Http2Settings());
        
        /**
         * @brief 添加自定义路由
         * 
         * @param pattern 路由模式（支持精确匹配）
         * @param handler 处理函数
         * 
         * @example
         * router.addRoute("/api/hello", [](Http2Connection& conn, uint32_t stream_id, 
         *                                   const std::string& method, const std::string& path,
         *                                   bool& handled) -> Coroutine<nil> {
         *     // 处理请求
         *     handled = true;
         *     co_return nil();
         * });
         */
        void addRoute(const std::string& pattern, RouteHandler handler);
        
        /**
         * @brief 路由请求
         * 
         * @param conn HTTP/2 连接对象
         * @param stream_id 流 ID
         * @param method HTTP 方法
         * @param path 请求路径
         * @param handled [out] 是否被处理
         */
        Coroutine<nil> route(Http2Connection& conn,
                            uint32_t stream_id,
                            const std::string& method,
                            const std::string& path,
                            bool& handled);
        
    private:
        struct MountPoint {
            std::string directory;
            FileTransferProgressCallback callback;
            Http2Settings settings;
        };
        
        struct Route {
            std::string pattern;
            RouteHandler handler;
        };
        
        // 挂载点映射（URL 前缀 -> 本地目录）
        std::map<std::string, MountPoint> m_mounts;
        
        // 自定义路由
        std::vector<Route> m_routes;
        
        /**
         * @brief 处理静态文件请求
         */
        Coroutine<nil> handleStaticFile(Http2Connection& conn,
                                       uint32_t stream_id,
                                       const std::string& method,
                                       const std::string& path,
                                       const MountPoint& mount_point,
                                       bool& handled);
        
        /**
         * @brief 发送文件
         */
        Coroutine<nil> sendFile(Http2Connection& conn,
                               uint32_t stream_id,
                               const std::string& file_path,
                               const FileTransferProgressCallback& callback,
                               const Http2Settings& settings);
        
        /**
         * @brief 发送错误响应
         */
        Coroutine<nil> sendError(Http2Connection& conn,
                                uint32_t stream_id,
                                int status_code,
                                const std::string& message);
    };
}

#endif // GALAY_HTTP2_ROUTER_H

