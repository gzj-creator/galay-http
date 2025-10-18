#ifndef GALAY_HTTP_PARAMS_H
#define GALAY_HTTP_PARAMS_H 

#include "galay-http/protoc/http/HttpBase.h"
#include <cstddef>  // for SIZE_MAX

namespace galay::http
{

#define DEFAULT_HTTP_RECV_TIMEOUT  std::chrono::milliseconds(30000);
#define DEFAULT_HTTP_SEND_TIMEOUT  std::chrono::milliseconds(30000);

    struct HttpSettings {
        std::chrono::milliseconds recv_timeout = DEFAULT_HTTP_RECV_TIMEOUT;
        std::chrono::milliseconds send_timeout = DEFAULT_HTTP_SEND_TIMEOUT;

        size_t recv_incr_length     = DEFAULT_HTTP_PEER_STEP_SIZE;
        size_t max_header_size      = DEFAULT_HTTP_MAX_HEADER_SIZE;
        size_t chunk_buffer_size    = DEFAULT_HTTP_CHUNK_BUFFER_SIZE;
        
        // 静态文件传输模式
        // true:  使用 Transfer-Encoding: chunked (内存占用小，但浏览器无法显示完整进度)
        // false: 使用 Content-Length (浏览器显示完整进度，但需要一次性读取文件到内存)
        bool use_chunked_transfer   = true;
        
        // 是否使用 sendfile 零拷贝传输（仅 Linux）
        // true:  使用 sendfile 系统调用（性能最佳，零拷贝，浏览器显示完整进度）
        // false: 使用普通的 send 系统调用
        // 注意：启用后会自动设置 Content-Length，无论 use_chunked_transfer 的值
        bool use_sendfile            = false;
        
        // sendfile 的块大小（仅在 use_sendfile=true 时生效）
        // 注意：底层 sendfile 有自己的循环，每次可能只发送部分数据就返回 EAGAIN
        // 应用层的块大小应该设置得足够大，让底层有机会多次尝试
        // 推荐值：直接发送整个文件（SIZE_MAX），让底层循环自动处理
        size_t sendfile_chunk_size   = SIZE_MAX;  // 不分块，一次发送整个范围
        
        // 是否支持 HTTP Range 请求（断点续传）
        bool support_range           = true;
    };

}

#endif