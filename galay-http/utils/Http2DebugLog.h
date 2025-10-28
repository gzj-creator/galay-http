#ifndef GALAY_HTTP2_DEBUG_LOG_H
#define GALAY_HTTP2_DEBUG_LOG_H

#include "HttpLogger.h"

// HTTP/2 调试日志宏 - 通过 ENABLE_DEBUG 宏控制是否编译
// 当 ENABLE_DEBUG 未定义时，这些宏在编译时会被完全移除，零性能开销

#ifdef ENABLE_DEBUG
    // Debug 模式：启用所有日志
    #define HTTP2_LOG_DEBUG(...) \
        galay::http::HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug(__VA_ARGS__)
    
    #define HTTP2_LOG_INFO(...) \
        galay::http::HttpLogger::getInstance()->getLogger()->getSpdlogger()->info(__VA_ARGS__)
    
    #define HTTP2_LOG_WARN(...) \
        galay::http::HttpLogger::getInstance()->getLogger()->getSpdlogger()->warn(__VA_ARGS__)
    
    #define HTTP2_LOG_ERROR(...) \
        galay::http::HttpLogger::getInstance()->getLogger()->getSpdlogger()->error(__VA_ARGS__)
#else
    // Release 模式：移除 debug 日志，保留 info 及以上级别
    #define HTTP2_LOG_DEBUG(...) ((void)0)  // 编译时完全移除
    
    #define HTTP2_LOG_INFO(...) \
        galay::http::HttpLogger::getInstance()->getLogger()->getSpdlogger()->info(__VA_ARGS__)
    
    #define HTTP2_LOG_WARN(...) \
        galay::http::HttpLogger::getInstance()->getLogger()->getSpdlogger()->warn(__VA_ARGS__)
    
    #define HTTP2_LOG_ERROR(...) \
        galay::http::HttpLogger::getInstance()->getLogger()->getSpdlogger()->error(__VA_ARGS__)
#endif

#endif // GALAY_HTTP2_DEBUG_LOG_H

