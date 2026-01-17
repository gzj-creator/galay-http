#ifndef GALAY_HTTP_WRITER_H
#define GALAY_HTTP_WRITER_H

#include "HttpWriterSetting.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpError.h"
#include "galay-kernel/kernel/Awaitable.h"
#include "galay-kernel/kernel/Timeout.hpp"
#include "galay-kernel/async/TcpSocket.h"
#include <expected>
#include <coroutine>
#include <string>

namespace galay::http
{

using namespace galay::kernel;
using namespace galay::async;

// 前向声明
class HttpWriter;

/**
 * @brief HTTP响应写入等待体
 * @details 用于异步写入HTTP响应，支持断点续传
 *
 * @note 支持超时设置：
 * @code
 * auto result = co_await writer.sendResponse(response).timeout(std::chrono::seconds(5));
 * @endcode
 */
class SendResponseAwaitable : public galay::kernel::TimeoutSupport<SendResponseAwaitable>
{
public:
    /**
     * @brief 构造函数
     * @param writer HttpWriter引用
     * @param send_awaitable SendAwaitable右值引用
     */
    SendResponseAwaitable(HttpWriter& writer, SendAwaitable&& send_awaitable)
        : m_writer(writer)
        , m_send_awaitable(std::move(send_awaitable))
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    auto await_suspend(std::coroutine_handle<> handle) {
        return m_send_awaitable.await_suspend(handle);
    }

    std::expected<bool, HttpError> await_resume();

private:
    HttpWriter& m_writer;
    SendAwaitable m_send_awaitable;

public:
    // TimeoutSupport 需要访问此成员来设置超时错误
    std::expected<bool, galay::kernel::IOError> m_result;
};

/**
 * @brief HTTP写入器
 * @details 提供异步写入HTTP响应的接口，支持断点续传
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
        , m_remaining_bytes(0)
    {
    }

    /**
     * @brief 发送HTTP响应
     * @param response HttpResponse引用
     * @return SendResponseAwaitable 响应等待体
     */
    SendResponseAwaitable sendResponse(HttpResponse& response) {
        // 只在第一次调用时（剩余字节为0）生成字符串
        if (m_remaining_bytes == 0) {
            m_buffer = response.toString();
            m_remaining_bytes = m_buffer.size();
        }

        // 计算当前发送位置
        size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
        const char* send_ptr = m_buffer.data() + sent_bytes;

        return SendResponseAwaitable(*this, m_socket.send(send_ptr, m_remaining_bytes));
    }

    /**
     * @brief 发送HTTP请求
     * @param request HttpRequest引用
     * @return SendResponseAwaitable 响应等待体
     */
    SendResponseAwaitable sendRequest(HttpRequest& request) {
        // 只在第一次调用时（剩余字节为0）生成字符串
        if (m_remaining_bytes == 0) {
            m_buffer = request.toString();
            m_remaining_bytes = m_buffer.size();
        }

        // 计算当前发送位置
        size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
        const char* send_ptr = m_buffer.data() + sent_bytes;

        return SendResponseAwaitable(*this, m_socket.send(send_ptr, m_remaining_bytes));
    }

    /**
     * @brief 发送HTTP响应头
     * @param header HttpResponseHeader右值引用
     * @return SendResponseAwaitable 响应等待体
     */
    SendResponseAwaitable sendHeader(HttpResponseHeader&& header) {
        // 只在第一次调用时（剩余字节为0）生成字符串
        if (m_remaining_bytes == 0) {
            m_buffer = header.toString();
            m_remaining_bytes = m_buffer.size();
        }

        // 计算当前发送位置
        size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
        const char* send_ptr = m_buffer.data() + sent_bytes;

        return SendResponseAwaitable(*this, m_socket.send(send_ptr, m_remaining_bytes));
    }

    /**
     * @brief 发送HTTP请求头
     * @param header HttpRequestHeader右值引用
     * @return SendResponseAwaitable 响应等待体
     */
    SendResponseAwaitable sendHeader(HttpRequestHeader&& header) {
        // 只在第一次调用时（剩余字节为0）生成字符串
        if (m_remaining_bytes == 0) {
            m_buffer = header.toString();
            m_remaining_bytes = m_buffer.size();
        }

        // 计算当前发送位置
        size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
        const char* send_ptr = m_buffer.data() + sent_bytes;

        return SendResponseAwaitable(*this, m_socket.send(send_ptr, m_remaining_bytes));
    }

    /**
     * @brief 发送字符串数据
     * @param data 字符串右值引用
     * @return SendResponseAwaitable 响应等待体
     */
    SendResponseAwaitable send(std::string&& data) {
        // 只在第一次调用时（剩余字节为0）赋值
        if (m_remaining_bytes == 0) {
            m_buffer = std::move(data);
            m_remaining_bytes = m_buffer.size();
        }

        // 计算当前发送位置
        size_t sent_bytes = m_buffer.size() - m_remaining_bytes;
        const char* send_ptr = m_buffer.data() + sent_bytes;

        return SendResponseAwaitable(*this, m_socket.send(send_ptr, m_remaining_bytes));
    }

    /**
     * @brief 发送原始数据（不支持断点续传，调用方需保证buffer生命周期）
     * @param buffer 数据缓冲区指针
     * @param length 数据长度
     * @return SendResponseAwaitable 响应等待体
     */
    SendResponseAwaitable send(const char* buffer, size_t length) {
        // 原始数据发送，不使用内部buffer
        // 注意：这个接口不支持断点续传，需要一次发送完成
        return SendResponseAwaitable(*this, m_socket.send(buffer, length));
    }

    /**
     * @brief 发送chunk编码的数据
     * @param data 要发送的数据
     * @param is_last 是否是最后一个chunk
     * @return SendResponseAwaitable 响应等待体
     */
    SendResponseAwaitable sendChunk(const std::string& data, bool is_last = false);

    /**
     * @brief 更新剩余发送字节数
     * @param bytes_sent 本次发送的字节数
     */
    void updateRemaining(size_t bytes_sent) {
        if (bytes_sent >= m_remaining_bytes) {
            m_remaining_bytes = 0;
            m_buffer.clear();  // 发送完成，清空buffer
        } else {
            m_remaining_bytes -= bytes_sent;
        }
    }

    /**
     * @brief 获取剩余发送字节数
     */
    size_t getRemainingBytes() const {
        return m_remaining_bytes;
    }

private:
    const HttpWriterSetting& m_setting;
    TcpSocket& m_socket;
    std::string m_buffer;        // 发送缓冲区
    size_t m_remaining_bytes;    // 剩余发送字节数
};

} // namespace galay::http

#endif // GALAY_HTTP_WRITER_H
