#ifndef GALAY_HTTP_DEBUG_LOG_H
#define GALAY_HTTP_DEBUG_LOG_H

// 必须在包含 spdlog 之前定义，以启用源代码位置捕获
#ifndef SPDLOG_ACTIVE_LEVEL
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include "HttpLogger.h"
#include <spdlog/spdlog.h>

// 调试日志宏 - 通过 ENABLE_DEBUG 宏控制是否编译
// 当 ENABLE_DEBUG 未定义时，这些宏在编译时会被完全移除，零性能开销
// 显式传递源代码位置（文件名和行号）

#ifdef ENABLE_DEBUG
    // Debug 模式：启用所有日志
    #define HTTP_LOG_DEBUG(...) \
        SPDLOG_LOGGER_DEBUG(galay::http::HttpLogger::getInstance()->getLogger()->getSpdlogger(), __VA_ARGS__)
    
    #define HTTP_LOG_INFO(...) \
        SPDLOG_LOGGER_INFO(galay::http::HttpLogger::getInstance()->getLogger()->getSpdlogger(), __VA_ARGS__)
    
    #define HTTP_LOG_WARN(...) \
        SPDLOG_LOGGER_WARN(galay::http::HttpLogger::getInstance()->getLogger()->getSpdlogger(), __VA_ARGS__)
    
    #define HTTP_LOG_ERROR(...) \
        SPDLOG_LOGGER_ERROR(galay::http::HttpLogger::getInstance()->getLogger()->getSpdlogger(), __VA_ARGS__)
#else
    // Release 模式：移除 debug 日志，保留 info 及以上级别
    #define HTTP_LOG_DEBUG(...) ((void)0)  // 编译时完全移除
    
    #define HTTP_LOG_INFO(...) \
        SPDLOG_LOGGER_INFO(galay::http::HttpLogger::getInstance()->getLogger()->getSpdlogger(), __VA_ARGS__)
    
    #define HTTP_LOG_WARN(...) \
        SPDLOG_LOGGER_WARN(galay::http::HttpLogger::getInstance()->getLogger()->getSpdlogger(), __VA_ARGS__)
    
    #define HTTP_LOG_ERROR(...) \
        SPDLOG_LOGGER_ERROR(galay::http::HttpLogger::getInstance()->getLogger()->getSpdlogger(), __VA_ARGS__)
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

#endif // GALAY_HTTP_DEBUG_LOG_H

