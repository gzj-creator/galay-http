#ifndef GALAY_HTTP_LOGGER_H
#define GALAY_HTTP_LOGGER_H 

#include "galay-http/protoc/HttpBase.h"

namespace galay::http
{
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
        case Http_Method_Get:     return "\033[32m"; // 绿色 - 安全操作
        case Http_Method_Post:    return "\033[33m"; // 黄色 - 数据修改
        case Http_Method_Put:     return "\033[34m"; // 蓝色 - 更新操作  
        case Http_Method_Delete:  return "\033[31m"; // 红色 - 危险操作
        case Http_Method_Head:    return "\033[36m"; // 青色 - 元数据操作
        case Http_Method_Options: return "\033[35m"; // 品红 - 调试用途
        case Http_Method_Patch:   return "\033[35;1m"; // 亮品红 - 部分更新
        case Http_Method_Trace:   return "\033[37m"; // 灰色 - 诊断用途
        case Http_Method_Connect: return "\033[33;1m"; // 亮黄色 - 隧道连接
        case Http_Method_Unknown: 
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
        return DEFAULT_LOG_METHOD_LENGTH;
    }

    inline int uri_length(const std::string& uri) 
    {
        int length = uri.length() + 2;
        return (length / DEFAULT_LOG_URI_PEER_LIMIT + 1) * DEFAULT_LOG_URI_PEER_LIMIT; 
    }

    inline int status_length(HttpStatusCode code)
    {
        return DEFAULT_LOG_STATUS_LENGTH;
    }

    inline int status_code_length(HttpStatusCode code)
    {
        return DEFAULT_LOG_STATUS_TEXT_LENGTH;
    }

    #define SERVER_REQUEST_LOG(METHOD, URI) {\
        std::string method = fmt::format("[{}{}{}]", method_color(METHOD), httpMethodToString(METHOD), RESET_COLOR);\
        std::string uri = fmt::format("[{}{}{}]", method_color(METHOD), URI, RESET_COLOR);\
        HttpLogger::getInstance()->getLogger()->getSpdlogger()->info( \
        "{:<{}} {:<{}}", \
        method, method_length(METHOD), \
        uri, uri_length(URI)); }

    #define SERVER_RESPONSE_DURING_LOG(STATUS, DURING_MS)   {\
        std::string status = fmt::format("[{}{}{}]", status_color(STATUS), std::to_string(static_cast<int>(STATUS)), RESET_COLOR);\
        std::string status_text = fmt::format("[{}{}{}]", status_color(STATUS), httpStatusCodeToString(STATUS), RESET_COLOR);\
        HttpLogger::getInstance()->getLogger()->getSpdlogger()->info( \
        "{:<{}} {:<{}} [{}During: {}ms{}]", \
        status, status_length(STATUS),\
        status_text, status_code_length(STATUS), \
        resp_time_color(DURING_MS), std::to_string(DURING_MS), RESET_COLOR); }

    #define SERVER_RESPONSE_LOG(STATUS)  {\
        std::string status = fmt::format("[{}{}{}]", status_color(STATUS), std::to_string(static_cast<int>(STATUS)), RESET_COLOR);\
        std::string status_text = fmt::format("[{}{}{}]", status_color(STATUS), httpStatusCodeToString(STATUS), RESET_COLOR);\
        HttpLogger::getInstance()->getLogger()->getSpdlogger()->info( \
        "{:<{}} {:<{}}", \
        status, status_length(STATUS),\
        status_text, status_code_length(STATUS)); }

    #define CLIENT_REQUEST_LOG(METHOD, URI) SERVER_REQUEST_LOG(METHOD, URI)
    #define CLIENT_RESPONSE_LOG(STATUS)  SERVER_RESPONSE_LOG(STATUS)

}

#endif