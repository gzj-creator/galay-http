#ifndef GALAY_HTTP_PARAMS_H
#define GALAY_HTTP_PARAMS_H 

/**
 * @file HttpParams.hpp
 * @brief HTTP 连接配置参数定义
 * 
 * 该文件定义了 HTTP 协议处理过程中使用的各种配置参数，包括：
 * - 超时设置（接收/发送超时）
 * - 缓冲区大小配置
 * - 静态文件传输模式（chunked、sendfile、Range 支持等）
 * 
 * @example 基本使用示例
 * @code
 * // 示例 1：创建默认配置
 * HttpSettings settings;
 * auto reader = conn.getReader(settings);
 * auto writer = conn.getWriter(settings);
 * 
 * // 示例 2：自定义超时和缓冲区
 * HttpSettings custom_settings;
 * custom_settings.recv_timeout = std::chrono::milliseconds(5000);  // 5秒超时
 * custom_settings.send_timeout = std::chrono::milliseconds(5000);
 * custom_settings.chunk_buffer_size = 64 * 1024;  // 64KB 缓冲区
 * @endcode
*/

#include "galay-http/protoc/http/HttpBase.h"
#include <cstddef>      // for SIZE_MAX
#include <functional>   // for std::function
#include <string>

namespace galay::http
{

// 前向声明
class HttpRequest;

// HTTP 默认超时配置（30秒）
#define DEFAULT_HTTP_RECV_TIMEOUT  std::chrono::milliseconds(30000);
#define DEFAULT_HTTP_SEND_TIMEOUT  std::chrono::milliseconds(30000);

    /**
     * @brief 文件传输详细信息
     * 
     * 在文件传输过程中提供给回调函数的文件信息
     */
    struct FileTransferInfo {
        std::string file_path;       ///< 完整文件路径（绝对路径）
        std::string relative_path;   ///< 相对于挂载点的路径（用户请求的路径）
        std::string mime_type;       ///< MIME 类型（如 "text/html", "image/png"）
        size_t file_size;            ///< 文件总大小（字节）
        size_t range_start;          ///< Range 请求的起始位置（0 表示从头开始）
        size_t range_end;            ///< Range 请求的结束位置（file_size-1 表示到文件末尾）
        bool is_range_request;       ///< 是否是 HTTP Range 请求（断点续传）
        
        /// 获取实际传输的字节数（Range 请求可能小于文件总大小）
        size_t getTransferSize() const {
            return range_end - range_start + 1;
        }
    };

    /**
     * @brief 文件传输进度回调函数类型
     * 
     * @param request HTTP 请求对象（包含请求头、URI、查询参数等）
     * @param bytes_sent 已发送的字节数
     * @param total_bytes 总共需要发送的字节数（对于 Range 请求，这是 Range 的大小）
     * @param file_info 文件详细信息
     * 
     * @note 此回调可能在传输过程中被多次调用
     * @note 回调应该尽快返回，避免阻塞传输过程
     * @note 在 chunked 传输模式下，每发送一块数据就会调用一次
     * @note 在 sendfile 模式下，可能只在开始和结束时调用
     * 
     * @example 进度条示例
     * @code
     * auto callback = [](const HttpRequest& req, size_t sent, size_t total, const FileTransferInfo& info) {
     *     double progress = (sent * 100.0) / total;
     *     std::cout << info.relative_path << ": " << progress << "% (" << sent << "/" << total << ")" << std::endl;
     * };
     * 
     * router.mount("/files", "./uploads", callback);
     * @endcode
     */
    using FileTransferProgressCallback = std::function<void(
        const HttpRequest&,           // HTTP 请求
        size_t,                       // 已发送字节数
        size_t,                       // 总字节数
        const FileTransferInfo&       // 文件信息
    )>;

    /**
     * @brief HTTP 连接配置参数
     * 
     * 该结构体包含了 HTTP 连接的各种配置选项，包括超时设置、缓冲区大小、
     * 传输模式等。可用于 HttpReader、HttpWriter 以及静态文件服务。
     * 
     * @note 所有参数都有合理的默认值，可以直接使用默认构造的实例
     * @note 对于静态文件服务，推荐在 Linux 上使用 use_sendfile=true 以获得最佳性能
     */
    struct HttpSettings {
        // ==================== 超时设置 ====================
        
        /// 接收数据超时时间（默认 30 秒）
        /// 如果在此时间内没有收到数据，recv 操作将超时返回
        std::chrono::milliseconds recv_timeout = DEFAULT_HTTP_RECV_TIMEOUT;
        
        /// 发送数据超时时间（默认 30 秒）
        /// 如果在此时间内无法发送数据，send 操作将超时返回
        std::chrono::milliseconds send_timeout = DEFAULT_HTTP_SEND_TIMEOUT;

        // ==================== 缓冲区设置 ====================
        
        /// 接收缓冲区增量大小（每次扩容的步长）
        /// 当缓冲区不足时，按此大小增加容量
        /// 默认值：DEFAULT_HTTP_PEER_STEP_SIZE（通常为 4KB 或 8KB）
        size_t recv_incr_length     = DEFAULT_HTTP_PEER_STEP_SIZE;
        
        /// HTTP 请求头最大允许大小
        /// 防止恶意请求发送超大 Header 导致内存耗尽
        /// 默认值：DEFAULT_HTTP_MAX_HEADER_SIZE（通常为 64KB 或 128KB）
        size_t max_header_size      = DEFAULT_HTTP_MAX_HEADER_SIZE;
        
        /// Chunked 传输编码的缓冲区大小
        /// 使用 Transfer-Encoding: chunked 时，每次读取/发送的块大小
        /// 默认值：DEFAULT_HTTP_CHUNK_BUFFER_SIZE（通常为 8KB 或 64KB）
        size_t chunk_buffer_size    = DEFAULT_HTTP_CHUNK_BUFFER_SIZE;
        
        // ==================== 静态文件传输设置 ====================
        
        /**
         * @brief 静态文件传输模式
         * 
         * - true:  使用 Transfer-Encoding: chunked
         *          优点：内存占用小（流式传输）
         *          缺点：浏览器无法显示下载进度条（不知道总大小）
         * 
         * - false: 使用 Content-Length
         *          优点：浏览器显示完整下载进度
         *          缺点：需要先获取文件大小（对于本地文件无影响）
         * 
         * 注意：如果启用了 use_sendfile，此选项将被忽略，强制使用 Content-Length
         */
        bool use_chunked_transfer   = true;
        
        /**
         * @brief 是否使用 sendfile 零拷贝传输（仅 Linux）
         * 
         * - true:  使用 sendfile(2) 系统调用
         *          性能最佳：零拷贝，数据直接从文件系统缓存发送到 socket
         *          自动设置 Content-Length，浏览器显示完整进度
         *          适用场景：高性能静态文件服务器
         * 
         * - false: 使用普通的 read + send 方式
         *          兼容性好：适用于所有平台（macOS、Windows、Linux）
         *          性能较低：需要在用户空间和内核空间之间拷贝数据
         * 
         * 注意：启用后会自动设置 Content-Length，无论 use_chunked_transfer 的值
         */
        bool use_sendfile            = false;
        
        /**
         * @brief sendfile 的单次发送大小（仅在 use_sendfile=true 时生效）
         * 
         * 说明：
         * - 底层 sendfile 系统调用有自己的发送循环
         * - 每次调用可能只发送部分数据就返回（例如 socket 缓冲区满时返回 EAGAIN）
         * - 应用层应该设置足够大的块大小，让底层有足够的发送机会
         * 
         * 推荐配置：
         * - SIZE_MAX（默认）：一次性发送整个文件范围，由底层自动分批发送
         * - 较小值（如 64KB）：仅在需要精细控制发送粒度时使用
         * 
         * 默认值：SIZE_MAX（不分块，一次发送整个文件范围）
         */
        size_t sendfile_chunk_size   = SIZE_MAX;
        
        /**
         * @brief 是否支持 HTTP Range 请求（断点续传）
         * 
         * - true:  支持 Range 头，允许客户端请求文件的特定范围
         *          响应状态码：206 Partial Content
         *          响应头：Content-Range: bytes start-end/total
         *          使用场景：视频播放、大文件下载、断点续传
         * 
         * - false: 忽略 Range 头，始终返回完整文件
         *          响应状态码：200 OK
         *          响应头：Content-Length: total
         * 
         * 默认值：true
         */
        bool support_range           = true;
    };

}

#endif