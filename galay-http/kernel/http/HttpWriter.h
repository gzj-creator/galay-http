#ifndef GALAY_HTTP_WRITER_H
#define GALAY_HTTP_WRITER_H

#include "HttpWriterSetting.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/protoc/http/HttpError.h"
#include "galay-kernel/kernel/Awaitable.h"
#include "galay-kernel/async/TcpSocket.h"
#include <expected>
#include <coroutine>
#include <string>

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
     * @param response_str 响应字符串（需要保持生命周期）
     * @param send_awaitable SendAwaitable右值引用
     */
    ResponseAwaitable(std::string&& response_str, SendAwaitable&& send_awaitable)
        : m_response_str(std::move(response_str))
        , m_send_awaitable(std::move(send_awaitable))
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    auto await_suspend(std::coroutine_handle<> handle) {
        return m_send_awaitable.await_suspend(handle);
    }

    std::expected<size_t, HttpError> await_resume();

private:
    std::string m_response_str;  // 保持响应字符串的生命周期
    SendAwaitable m_send_awaitable;
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
     * @param setting HttpWriterSetting引用，包含写入配置
     * @param socket TcpSocket引用，用于IO操作
     */
    HttpWriter(const HttpWriterSetting& setting, TcpSocket& socket)
        : m_setting(setting)
        , m_socket(socket)
    {
    }

    /**
     * @brief 发送HTTP响应
     * @param response HttpResponse引用
     * @return ResponseAwaitable 响应等待体
     */
    ResponseAwaitable sendResponse(HttpResponse& response) {
        // 将响应序列化为字符串
        std::string responseStr = response.toString();

        // 返回 ResponseAwaitable，内部持有 responseStr 保持生命周期
        return ResponseAwaitable(std::move(responseStr),
                                m_socket.send(responseStr.data(), responseStr.size()));
    }

private:
    const HttpWriterSetting& m_setting;
    TcpSocket& m_socket;
};

} // namespace galay::http

#endif // GALAY_HTTP_WRITER_H
