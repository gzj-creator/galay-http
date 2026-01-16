#ifndef GALAY_HTTP_WRITER_SETTING_H
#define GALAY_HTTP_WRITER_SETTING_H

#include "galay-http/protoc/http/HttpBase.h"
#include <cstddef>

namespace galay::http
{

/**
 * @brief HTTP写入器配置类
 * @details 用于配置HttpWriter的各种参数
 */
class HttpWriterSetting
{
public:
    HttpWriterSetting() = default;
    ~HttpWriterSetting() = default;

    /**
     * @brief 设置发送超时时间
     * @param timeout_ms 超时时间（毫秒）
     */
    void setSendTimeout(int timeout_ms) {
        m_send_timeout_ms = timeout_ms;
    }

    /**
     * @brief 获取发送超时时间
     * @return 超时时间（毫秒）
     */
    int getSendTimeout() const {
        return m_send_timeout_ms;
    }

    /**
     * @brief 设置是否启用缓冲
     * @param enable 是否启用
     */
    void setBufferingEnabled(bool enable) {
        m_buffering_enabled = enable;
    }

    /**
     * @brief 获取是否启用缓冲
     * @return 是否启用
     */
    bool isBufferingEnabled() const {
        return m_buffering_enabled;
    }

    /**
     * @brief 设置最大响应大小
     * @param max_size 最大响应大小（字节）
     */
    void setMaxResponseSize(size_t max_size) {
        m_max_response_size = max_size;
    }

    /**
     * @brief 获取最大响应大小
     * @return 最大响应大小（字节）
     */
    size_t getMaxResponseSize() const {
        return m_max_response_size;
    }

private:
    int m_send_timeout_ms = DEFAULT_HTTP_SEND_TIME_MS;
    bool m_buffering_enabled = true;
    size_t m_max_response_size = DEFAULT_HTTP_MAX_BODY_SIZE;
};

} // namespace galay::http

#endif // GALAY_HTTP_WRITER_SETTING_H
