#include "HttpLogger.h"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/async_logger.h>

namespace galay::http
{ 
    HttpLogger::HttpLogger()
    {
        m_thread_pool = std::make_shared<spdlog::details::thread_pool>( DEFAULT_LOG_QUEUE_SIZE, DEFAULT_LOG_THREADS);
        auto logger = std::make_shared<spdlog::async_logger>("galay", std::make_shared<spdlog::sinks::stdout_color_sink_mt>(), m_thread_pool);
        logger->set_level(spdlog::level::level_enum::info);
        logger->set_pattern("[%Y-%m-%d %T.%e] [%^%L%$] %v");
        m_logger = std::make_unique<Logger>(logger);
    }   

    HttpLogger *HttpLogger::getInstance()
    {
        static HttpLogger instance;
        return &instance;
    }

}