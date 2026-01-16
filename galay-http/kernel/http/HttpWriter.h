#ifndef GALAY_HTTP_WRITER_H
#define GALAY_HTTP_WRITER_H

#include "HttpWriterSetting.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/protoc/http/HttpError.h"
#include "galay-kernel/kernel/Awaitable.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/common/Buffer.h"
#include <expected>
#include <coroutine>

namespace galay::http
{

using namespace galay::kernel;
using namespace galay::async;

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
    ResponseAwaitable(HttpResponse& response, WritevAwaitable&& writev_awaitable)
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
    HttpResponse& m_response;
    WritevAwaitable m_writev_awaitable;
};

/**
 * @brief HTTP写入器
 * @details 提供异步写入HTTP响应的接口
 */
class HttpWriter
{
public:
    /**
     * @brief 构造函数
     * @param ring_buffer RingBuffer引用，用于缓冲发送的数据
     * @param setting HttpWriterSetting引用，包含写入配置
     * @param socket TcpSocket引用，用于IO操作
     */
    HttpWriter(RingBuffer& ring_buffer, const HttpWriterSetting& setting, TcpSocket& socket)
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_socket(socket)
    {
    }

    /**
     * @brief 发送HTTP响应
     * @param response HttpResponse引用
     * @return ResponseAwaitable 响应等待体
     */
    ResponseAwaitable sendResponse(HttpResponse& response) {
        // 将响应序列化到 RingBuffer
        std::string responseStr = response.toString();
        m_ring_buffer.write(responseStr.data(), responseStr.size());

        // 获取可读的 iovec 并发送
        auto readIovecs = m_ring_buffer.getReadIovecs();
        return ResponseAwaitable(response, m_socket.writev(std::move(readIovecs)));
    }

private:
    RingBuffer& m_ring_buffer;
    const HttpWriterSetting& m_setting;
    TcpSocket& m_socket;
};

} // namespace galay::http

#endif // GALAY_HTTP_WRITER_H
