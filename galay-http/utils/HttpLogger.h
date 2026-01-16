#ifndef GALAY_HTTP_LOGGER_H
#define GALAY_HTTP_LOGGER_H

#include "galay-http/protoc/http/HttpBase.h"
#include "galay-kernel/common/Log.h"
#include <memory>
#include <string>
#include <spdlog/spdlog.h>
#include <spdlog/async.h>

namespace galay::http
{
    using kernel::Logger;

    class HttpLogger
    {
    public:
        using uptr = std::unique_ptr<HttpLogger>;

        HttpLogger();

        static HttpLogger* getInstance();
        Logger* getLogger() {
            return m_logger.get();
        }
        void resetLogger(std::unique_ptr<Logger> logger) {
            m_logger = std::move(logger);
        }
    private:
        std::unique_ptr<Logger> m_logger;
        std::shared_ptr<spdlog::details::thread_pool> m_thread_pool;
    };


    const spdlog::string_view_t RESET_COLOR = "\033[0m";
    const spdlog::string_view_t GRAY_COLOR = "\033[37m";

    inline spdlog::string_view_t status_color(HttpStatusCode status_code) {
        using enum HttpStatusCode;
        int status = static_cast<int>(status_code);
        if (status >= 100 && status < 200) { // 1xx Informational
            return "\033[36m";  // 青色
        } else if (status >= 200 && status < 300) { // 2xx Success
            return "\033[32m";  // 绿色
        } else if (status >= 300 && status < 400) { // 3xx Redirection
            return "\033[33m";  // 黄色
        } else if (status >= 400 && status < 500) { // 4xx Client Error
            return "\033[31m";  // 红色
        } else if (status >= 500) { // 5xx Server Error
            return "\033[35m";  // 品红
        }
        return "\033[90m"; // 未知状态码用暗灰色
    }

    inline spdlog::string_view_t method_color(HttpMethod method) {
        using enum HttpMethod;

        switch (method) {
        case HttpMethod_Get:     return "\033[32m"; // 绿色 - 安全操作
        case HttpMethod_Post:    return "\033[33m"; // 黄色 - 数据修改
        case HttpMethod_Put:     return "\033[34m"; // 蓝色 - 更新操作
        case HttpMethod_Delete:  return "\033[31m"; // 红色 - 危险操作
        case HttpMethod_Head:    return "\033[36m"; // 青色 - 元数据操作
        case HttpMethod_Options: return "\033[35m"; // 品红 - 调试用途
        case HttpMethod_Patch:   return "\033[35;1m"; // 亮品红 - 部分更新
        case HttpMethod_Trace:   return "\033[37m"; // 灰色 - 诊断用途
        case HttpMethod_Connect: return "\033[33;1m"; // 亮黄色 - 隧道连接
        case HttpMethod_PRI:     return "\033[36;1m"; // 亮青色 - HTTP/2 升级
        case HttpMethod_Unknown:
        default:                  return "\033[90m"; // 暗灰色 - 未知方法
        }
        return "\033[0m";
    }

    inline spdlog::string_view_t resp_time_color(size_t ms) {
        if (ms < 100) return "\033[32m";      // 绿色：优秀性能
        if (ms < 500) return "\033[33m";     // 黄色：需关注
        return "\033[31m";                   // 红色：严重延迟
    }

    inline int method_length(HttpMethod method)
    {
        (void)method;
        return DEFAULT_LOG_METHOD_LENGTH;
    }

    inline int uri_length(const std::string& uri)
    {
        int length = uri.length() + 2;
        // 限制最大宽度为 60，避免终端换行
        int aligned = (length / DEFAULT_LOG_URI_PEER_LIMIT + 1) * DEFAULT_LOG_URI_PEER_LIMIT;
        return std::min(aligned, 60);
    }

    inline int status_length(HttpStatusCode code)
    {
        (void)code;
        return DEFAULT_LOG_STATUS_LENGTH;
    }

    inline int status_code_length(HttpStatusCode code)
    {
        (void)code;
        return DEFAULT_LOG_STATUS_TEXT_LENGTH;
    }



}

#endif
