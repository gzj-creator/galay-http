#ifndef GALAY_HTTP_READER_H
#define GALAY_HTTP_READER_H

#include "HttpReaderSetting.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpError.h"
#include "galay-kernel/common/Buffer.h"
#include "galay-kernel/kernel/Awaitable.h"
#include <expected>
#include <coroutine>

namespace galay::http
{

using namespace galay::kernel;

/**
 * @brief HTTP请求读取等待体
 * @details 用于异步读取HTTP请求，内部使用ReadvAwaitable进行实际的IO操作
 */
class RequestAwaitable
{
public:
    /**
     * @brief 构造函数
     * @param ring_buffer RingBuffer引用，用于缓冲接收的数据
     * @param setting HttpReaderSetting引用，包含读取配置
     * @param request HttpRequest引用，用于存储解析结果
     * @param readv_awaitable ReadvAwaitable右值引用，用于实际的IO操作
     */
    RequestAwaitable(RingBuffer& ring_buffer,
                    const HttpReaderSetting& setting,
                    HttpRequest& request,
                    ReadvAwaitable&& readv_awaitable)
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_request(request)
        , m_readv_awaitable(std::move(readv_awaitable))
        , m_total_received(0)
    {
    }

    /**
     * @brief 检查是否准备就绪
     * @return 始终返回false，需要挂起协程
     */
    bool await_ready() const noexcept {
        return false;
    }

    /**
     * @brief 挂起协程
     * @param handle 协程句柄
     * @return 返回ReadvAwaitable的await_suspend结果
     */
    auto await_suspend(std::coroutine_handle<> handle) {
        return m_readv_awaitable.await_suspend(handle);
    }

    /**
     * @brief 恢复协程时调用
     * @return std::expected<bool, HttpError>
     *         - true: HttpRequest完整解析
     *         - false: 不完整，需要继续调用
     *         - HttpError: 解析错误
     */
    std::expected<bool, HttpError> await_resume();

private:
    RingBuffer& m_ring_buffer;
    const HttpReaderSetting& m_setting;
    HttpRequest& m_request;
    ReadvAwaitable m_readv_awaitable;
    size_t m_total_received;
};

/**
 * @brief HTTP读取器
 * @details 提供异步读取HTTP请求的接口
 */
class HttpReader
{
public:
    /**
     * @brief 构造函数
     * @param ring_buffer RingBuffer引用，用于缓冲接收的数据
     * @param setting HttpReaderSetting引用，包含读取配置
     */
    HttpReader(RingBuffer& ring_buffer, const HttpReaderSetting& setting)
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
    {
    }

    /**
     * @brief 获取一个完整的HTTP请求
     * @param request HttpRequest引用，用于存储解析结果
     * @param readv_awaitable ReadvAwaitable右值引用，用于实际的IO操作
     * @return RequestAwaitable 请求等待体
     */
    RequestAwaitable getRequest(HttpRequest& request, ReadvAwaitable&& readv_awaitable) {
        return RequestAwaitable(m_ring_buffer, m_setting, request, std::move(readv_awaitable));
    }

private:
    RingBuffer& m_ring_buffer;
    const HttpReaderSetting& m_setting;
};

} // namespace galay::http

#endif // GALAY_HTTP_READER_H
