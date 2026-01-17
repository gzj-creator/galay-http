#ifndef GALAY_HTTP_READER_H
#define GALAY_HTTP_READER_H

#include "HttpReaderSetting.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/protoc/http/HttpError.h"
#include "galay-kernel/common/Buffer.h"
#include "galay-kernel/kernel/Awaitable.h"
#include "galay-kernel/async/TcpSocket.h"
#include <expected>
#include <coroutine>

namespace galay::http
{

using namespace galay::kernel;
using namespace galay::async;

/**
 * @brief HTTP请求读取等待体
 * @details 用于异步读取HTTP请求，内部使用ReadvAwaitable进行实际的IO操作
 */
class GetRequestAwaitable
{
public:
    /**
     * @brief 构造函数
     * @param ring_buffer RingBuffer引用，用于缓冲接收的数据
     * @param setting HttpReaderSetting引用，包含读取配置
     * @param request HttpRequest引用，用于存储解析结果
     * @param readv_awaitable ReadvAwaitable右值引用，用于实际的IO操作
     */
    GetRequestAwaitable(RingBuffer& ring_buffer,
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
 * @brief HTTP响应读取等待体
 * @details 用于异步读取HTTP响应，内部使用ReadvAwaitable进行实际的IO操作
 */
class GetResponseAwaitable
{
public:
    /**
     * @brief 构造函数
     * @param ring_buffer RingBuffer引用，用于缓冲接收的数据
     * @param setting HttpReaderSetting引用，包含读取配置
     * @param response HttpResponse引用，用于存储解析结果
     * @param readv_awaitable ReadvAwaitable右值引用，用于实际的IO操作
     */
    GetResponseAwaitable(RingBuffer& ring_buffer,
                     const HttpReaderSetting& setting,
                     HttpResponse& response,
                     ReadvAwaitable&& readv_awaitable)
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_response(response)
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
     *         - true: HttpResponse完整解析
     *         - false: 不完整，需要继续调用
     *         - HttpError: 解析错误
     */
    std::expected<bool, HttpError> await_resume();

private:
    RingBuffer& m_ring_buffer;
    const HttpReaderSetting& m_setting;
    HttpResponse& m_response;
    ReadvAwaitable m_readv_awaitable;
    size_t m_total_received;
};

/**
 * @brief HTTP Chunk读取等待体
 * @details 用于异步读取HTTP chunked编码的数据块
 *          每次调用从RingBuffer消费一个或多个完整的chunk
 */
class GetChunkAwaitable
{
public:
    /**
     * @brief 构造函数
     * @param ring_buffer RingBuffer引用，用于缓冲接收的数据
     * @param setting HttpReaderSetting引用，包含读取配置
     * @param chunk_data 用户传入的string引用，用于接收chunk数据
     * @param readv_awaitable ReadvAwaitable右值引用，用于实际的IO操作
     */
    GetChunkAwaitable(RingBuffer& ring_buffer,
                  const HttpReaderSetting& setting,
                  std::string& chunk_data,
                  ReadvAwaitable&& readv_awaitable)
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_chunk_data(chunk_data)
        , m_readv_awaitable(std::move(readv_awaitable))
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
     *         - true: 读取到最后一个chunk (size=0)，所有chunk读取完成
     *         - false: 读取到chunk数据但不是最后一个，需要继续调用
     *         - HttpError: 解析错误
     */
    std::expected<bool, HttpError> await_resume();

private:
    RingBuffer& m_ring_buffer;
    const HttpReaderSetting& m_setting;
    std::string& m_chunk_data;
    ReadvAwaitable m_readv_awaitable;
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
     * @param socket TcpSocket引用，用于IO操作
     */
    HttpReader(RingBuffer& ring_buffer, const HttpReaderSetting& setting, TcpSocket& socket)
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_socket(socket)
    {
    }

    /**
     * @brief 获取一个完整的HTTP请求
     * @param request HttpRequest引用，用于存储解析结果
     * @return GetRequestAwaitable 请求等待体
     */
    GetRequestAwaitable getRequest(HttpRequest& request) {
        return GetRequestAwaitable(m_ring_buffer, m_setting, request,
                              m_socket.readv(m_ring_buffer.getWriteIovecs()));
    }

    /**
     * @brief 获取一个完整的HTTP响应
     * @param response HttpResponse引用，用于存储解析结果
     * @return GetResponseAwaitable 响应等待体
     */
    GetResponseAwaitable getResponse(HttpResponse& response) {
        return GetResponseAwaitable(m_ring_buffer, m_setting, response,
                                m_socket.readv(m_ring_buffer.getWriteIovecs()));
    }

    /**
     * @brief 获取HTTP chunked编码的数据块
     * @param chunk_data 用户传入的string引用，用于接收chunk数据
     * @return GetChunkAwaitable chunk等待体
     * @details 每次调用从RingBuffer消费一个或多个完整的chunk
     *          返回true表示读取到最后一个chunk，返回false表示需要继续调用
     */
    GetChunkAwaitable getChunk(std::string& chunk_data) {
        return GetChunkAwaitable(m_ring_buffer, m_setting, chunk_data,
                             m_socket.readv(m_ring_buffer.getWriteIovecs()));
    }

private:
    RingBuffer& m_ring_buffer;
    const HttpReaderSetting& m_setting;
    TcpSocket& m_socket;
};

} // namespace galay::http

#endif // GALAY_HTTP_READER_H
