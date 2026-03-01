#include "HttpLogger.h"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/async_logger.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace galay::http
{
    namespace {
    spdlog::level::level_enum defaultCompileLevel() {
#ifdef ENABLE_DEBUG
        return spdlog::level::debug;
#else
        return spdlog::level::info;
#endif
    }

    bool parseLevel(std::string name, spdlog::level::level_enum& out) {
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        if (name == "trace") { out = spdlog::level::trace; return true; }
        if (name == "debug") { out = spdlog::level::debug; return true; }
        if (name == "info")  { out = spdlog::level::info; return true; }
        if (name == "warn" || name == "warning") { out = spdlog::level::warn; return true; }
        if (name == "error") { out = spdlog::level::err; return true; }
        if (name == "critical") { out = spdlog::level::critical; return true; }
        if (name == "off") { out = spdlog::level::off; return true; }
        return false;
    }

    spdlog::level::level_enum resolveDefaultLevelFromEnv() {
        const char* env_level = std::getenv("GALAY_HTTP_LOG_LEVEL");
        if (env_level == nullptr || *env_level == '\0') {
            return defaultCompileLevel();
        }
        spdlog::level::level_enum parsed = defaultCompileLevel();
        if (!parseLevel(env_level, parsed)) {
            return defaultCompileLevel();
        }
        return parsed;
    }
    } // namespace

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

        m_spdlogger->set_level(resolveDefaultLevelFromEnv());
    }

    HttpLogger *HttpLogger::getInstance()
    {
        static HttpLogger instance;
        return &instance;
    }

    void HttpLogger::enable()
    {
        console();
    }

    void HttpLogger::console()
    {
        console("galay-http");
    }

    void HttpLogger::console(const std::string& logger_name)
    {
        auto instance = getInstance();
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

        instance->m_spdlogger = std::make_shared<spdlog::async_logger>(logger_name,
            console_sink,
            instance->m_thread_pool);

        instance->m_spdlogger->set_pattern("[%Y-%m-%d %T.%e] [%^%L%$] [%s:%#] %v");

        instance->m_spdlogger->set_level(resolveDefaultLevelFromEnv());
    }

    void HttpLogger::file(const std::string& log_file_path, const std::string& logger_name)
    {
        auto instance = getInstance();
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file_path, true);

        instance->m_spdlogger = std::make_shared<spdlog::async_logger>(logger_name,
            file_sink,
            instance->m_thread_pool);

        instance->m_spdlogger->set_pattern("[%Y-%m-%d %T.%e] [%^%L%$] [%s:%#] %v");

        instance->m_spdlogger->set_level(resolveDefaultLevelFromEnv());
    }

    void HttpLogger::setLogger(std::shared_ptr<spdlog::logger> logger)
    {
        auto instance = getInstance();
        instance->m_spdlogger = std::move(logger);
    }

    void HttpLogger::setLevel(spdlog::level::level_enum level)
    {
        auto instance = getInstance();
        if (instance->m_spdlogger) {
            instance->m_spdlogger->set_level(level);
        }
    }

    bool HttpLogger::setLevel(const std::string& level_name)
    {
        spdlog::level::level_enum level_value = spdlog::level::info;
        if (!parseLevel(level_name, level_value)) {
            return false;
        }
        setLevel(level_value);
        return true;
    }

    spdlog::level::level_enum HttpLogger::level()
    {
        auto instance = getInstance();
        if (!instance->m_spdlogger) {
            return spdlog::level::off;
        }
        return instance->m_spdlogger->level();
    }

    void HttpLogger::disable()
    {
        auto instance = getInstance();
        if (instance->m_spdlogger) {
            instance->m_spdlogger->set_level(spdlog::level::off);
        }
    }

}
