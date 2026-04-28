#ifndef GALAY_HTTP_LOG_H
#define GALAY_HTTP_LOG_H

// 必须在包含 spdlog 之前定义，以启用源代码位置捕获
#ifndef SPDLOG_ACTIVE_LEVEL
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include "galay-http/utils/http_logger.h"
#include <spdlog/spdlog.h>

// 日志宏说明：
// - GALAY_HTTP_DISABLE_ALL_LOG: 完全编译期裁剪（WS/HTTP/HTTPS/WSS/H2C/H2 全部日志）
// - GALAY_HTTP_DISABLE_DEBUG_LOG: 仅裁剪 debug 日志
#ifdef GALAY_HTTP_DISABLE_ALL_LOG
#define HTTP_LOG_DEBUG(...) ((void)0)
#define HTTP_LOG_INFO(...) ((void)0)
#define HTTP_LOG_WARN(...) ((void)0)
#define HTTP_LOG_ERROR(...) ((void)0)
#else
    #ifdef GALAY_HTTP_DISABLE_DEBUG_LOG
    #define HTTP_LOG_DEBUG(...) ((void)0)
    #else
    #define HTTP_LOG_DEBUG(...) \
        SPDLOG_LOGGER_DEBUG(galay::http::HttpLog::getInstance()->getSpdlogger(), __VA_ARGS__)
    #endif

    #define HTTP_LOG_INFO(...) \
        SPDLOG_LOGGER_INFO(galay::http::HttpLog::getInstance()->getSpdlogger(), __VA_ARGS__)

    #define HTTP_LOG_WARN(...) \
        SPDLOG_LOGGER_WARN(galay::http::HttpLog::getInstance()->getSpdlogger(), __VA_ARGS__)

    #define HTTP_LOG_ERROR(...) \
        SPDLOG_LOGGER_ERROR(galay::http::HttpLog::getInstance()->getSpdlogger(), __VA_ARGS__)
#endif

// 格式化输出宏 - 使用 HTTP_LOG_INFO 以显示源代码位置
#define SERVER_REQUEST_LOG(METHOD, URI) \
    HTTP_LOG_INFO("{:<{}} {:<{}}", \
        fmt::format("[{}{}{}]", galay::http::method_color(METHOD), galay::http::httpMethodToString(METHOD), galay::http::RESET_COLOR), galay::http::method_length(METHOD), \
        fmt::format("[{}{}{}]", galay::http::method_color(METHOD), URI, galay::http::RESET_COLOR), galay::http::uri_length(URI))

#define SERVER_RESPONSE_DURING_LOG(STATUS, DURING_MS) \
    HTTP_LOG_INFO("{:<{}} {:<{}} [{}During: {}ms{}]", \
        fmt::format("[{}{}{}]", galay::http::status_color(STATUS), std::to_string(static_cast<int>(STATUS)), galay::http::RESET_COLOR), galay::http::status_length(STATUS), \
        fmt::format("[{}{}{}]", galay::http::status_color(STATUS), galay::http::httpStatusCodeToString(STATUS), galay::http::RESET_COLOR), galay::http::status_code_length(STATUS), \
        galay::http::resp_time_color(DURING_MS), std::to_string(DURING_MS), galay::http::RESET_COLOR)

#define SERVER_RESPONSE_LOG(STATUS) \
    HTTP_LOG_INFO("{:<{}} {:<{}}", \
        fmt::format("[{}{}{}]", galay::http::status_color(STATUS), std::to_string(static_cast<int>(STATUS)), galay::http::RESET_COLOR), galay::http::status_length(STATUS), \
        fmt::format("[{}{}{}]", galay::http::status_color(STATUS), galay::http::httpStatusCodeToString(STATUS), galay::http::RESET_COLOR), galay::http::status_code_length(STATUS))

#define CLIENT_REQUEST_LOG(METHOD, URI) SERVER_REQUEST_LOG(METHOD, URI)
#define CLIENT_RESPONSE_LOG(STATUS)  SERVER_RESPONSE_LOG(STATUS)

#endif // GALAY_HTTP_LOG_H
