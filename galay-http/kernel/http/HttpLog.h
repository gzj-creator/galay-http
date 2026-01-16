#ifndef GALAY_HTTP_LOG_H
#define GALAY_HTTP_LOG_H

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <memory>

namespace galay
{
namespace http
{

// 日志管理类
class HttpLogManager
{
public:
    static HttpLogManager& instance() {
        static HttpLogManager manager;
        return manager;
    }

    std::shared_ptr<spdlog::logger> getLogger() {
        return m_logger;
    }

private:
    HttpLogManager() {
        // 创建带颜色的控制台logger
        m_logger = spdlog::stdout_color_mt("http");

        // 设置日志格式：[时间] [级别] [文件:行号] 消息
        m_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%s:%#] %v");

        // 设置日志级别
#ifdef ENABLE_DEBUG
        m_logger->set_level(spdlog::level::debug);
#else
        m_logger->set_level(spdlog::level::info);
#endif
    }

    ~HttpLogManager() {
        spdlog::drop("http");
    }

    std::shared_ptr<spdlog::logger> m_logger;
};

} // namespace http
} // namespace galay

// 日志宏定义
#ifdef ENABLE_DEBUG
    #define HTTP_LOG_DEBUG(...) \
        SPDLOG_LOGGER_DEBUG(galay::http::HttpLogManager::instance().getLogger(), __VA_ARGS__)
#else
    #define HTTP_LOG_DEBUG(...) ((void)0)
#endif

#define HTTP_LOG_INFO(...) \
    SPDLOG_LOGGER_INFO(galay::http::HttpLogManager::instance().getLogger(), __VA_ARGS__)

#define HTTP_LOG_WARN(...) \
    SPDLOG_LOGGER_WARN(galay::http::HttpLogManager::instance().getLogger(), __VA_ARGS__)

#define HTTP_LOG_ERROR(...) \
    SPDLOG_LOGGER_ERROR(galay::http::HttpLogManager::instance().getLogger(), __VA_ARGS__)

#endif // GALAY_HTTP_LOG_H
