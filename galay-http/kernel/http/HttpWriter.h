#ifndef GALAY_HTTP_WRITER_H
#define GALAY_HTTP_WRITER_H

#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/protoc/http/HttpError.h"
#include "galay-kernel/kernel/Awaitable.h"
#include <expected>
#include <coroutine>

namespace galay::http
{

using namespace galay::kernel;

/**
 * @brief HTTP响应写入等待体
 * @details 用于异步写入HTTP响应
 */
class ResponseAwaitable
{
public:
    /**
     * @brief 构造函数
     * @param response HttpResponse引用
     * @param writev_awaitable WritevAwaitable右值引用
     */
    ResponseAwaitable(const HttpResponse& response, WritevAwaitable&& writev_awaitable)
        : m_response(response)
        , m_writev_awaitable(std::move(writev_awaitable))
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    auto await_suspend(std::coroutine_handle<> handle) {
        return m_writev_awaitable.await_suspend(handle);
    }

    std::expected<size_t, HttpError> await_resume();

private:
    const HttpResponse& m_response;
    WritevAwaitable m_writev_awaitable;
};

/**
 * @brief HTTP写入器
 * @details 提供异步写入HTTP响应的接口
 */
class HttpWriter
{
public:
    HttpWriter() = default;
    ~HttpWriter() = default;

    /**
     * @brief 发送HTTP响应
     * @param response HttpResponse引用
     * @param writev_awaitable WritevAwaitable右值引用
     * @return ResponseAwaitable 响应等待体
     */
    ResponseAwaitable sendResponse(const HttpResponse& response, WritevAwaitable&& writev_awaitable) {
        return ResponseAwaitable(response, std::move(writev_awaitable));
    }
};

} // namespace galay::http

#endif // GALAY_HTTP_WRITER_H
