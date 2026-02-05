#include "HttpLogger.h"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/async_logger.h>

namespace galay::http
{
    HttpLogger::HttpLogger()
    {
        m_thread_pool = std::make_shared<spdlog::details::thread_pool>(DEFAULT_LOG_QUEUE_SIZE, DEFAULT_LOG_THREADS);

        // 默认使用文件日志
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("galay-http.log", true);
        m_spdlogger = std::make_shared<spdlog::async_logger>("galay-http",
            file_sink,
            m_thread_pool);

        // 设置日志格式：[时间] [级别] [文件:行号] 消息
        m_spdlogger->set_pattern("[%Y-%m-%d %T.%e] [%^%L%$] [%s:%#] %v");

        // 根据 ENABLE_DEBUG 宏设置日志级别
#ifdef ENABLE_DEBUG
        m_spdlogger->set_level(spdlog::level::debug);
#else
        m_spdlogger->set_level(spdlog::level::info);
#endif
    }

    HttpLogger *HttpLogger::getInstance()
    {
        static HttpLogger instance;
        return &instance;
    }

    void HttpLogger::console()
    {
        auto instance = getInstance();
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

        instance->m_spdlogger = std::make_shared<spdlog::async_logger>("galay-http",
            console_sink,
            instance->m_thread_pool);

        instance->m_spdlogger->set_pattern("[%Y-%m-%d %T.%e] [%^%L%$] [%s:%#] %v");

#ifdef ENABLE_DEBUG
        instance->m_spdlogger->set_level(spdlog::level::debug);
#else
        instance->m_spdlogger->set_level(spdlog::level::info);
#endif
    }

    void HttpLogger::file(const std::string& log_file_path)
    {
        auto instance = getInstance();
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file_path, true);

        instance->m_spdlogger = std::make_shared<spdlog::async_logger>("galay-http",
            file_sink,
            instance->m_thread_pool);

        instance->m_spdlogger->set_pattern("[%Y-%m-%d %T.%e] [%^%L%$] [%s:%#] %v");

#ifdef ENABLE_DEBUG
        instance->m_spdlogger->set_level(spdlog::level::debug);
#else
        instance->m_spdlogger->set_level(spdlog::level::info);
#endif
    }

    void HttpLogger::disable()
    {
        auto instance = getInstance();
        instance->m_spdlogger->set_level(spdlog::level::off);
    }

}