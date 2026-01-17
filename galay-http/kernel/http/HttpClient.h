#ifndef GALAY_HTTP_CLIENT_H
#define GALAY_HTTP_CLIENT_H

#include "HttpWriter.h"
#include "HttpReader.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/common/Buffer.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include <string>
#include <optional>
#include <coroutine>
#include <map>

namespace galay::http
{

using namespace galay::async;
using namespace galay::kernel;

// 前向声明
class HttpClient;

/**
 * @brief HTTP客户端等待体
 * @details 自动处理完整的请求发送和响应接收流程
 *          返回 std::expected<std::optional<HttpResponse>, HttpError>
 *          - HttpResponse: 请求和响应全部完成
 *          - std::nullopt: 需要继续调用（数据未完全发送或接收）
 *          - HttpError: 发生错误
 */
class HttpClientAwaitable
{
public:
    /**
     * @brief 构造函数
     * @param client HttpClient引用
     * @param request HttpRequest右值引用
     */
    HttpClientAwaitable(HttpClient& client, HttpRequest&& request);

    bool await_ready() const noexcept {
        return false;
    }

    bool await_suspend(std::coroutine_handle<> handle);

    std::expected<std::optional<HttpResponse>, HttpError> await_resume();

    /**
     * @brief 检查状态是否为 Invalid
     * @return true 如果状态为 Invalid
     */
    bool isInvalid() const {
        return m_state == State::Invalid;
    }

    /**
     * @brief 重置状态并清理资源
     * @details 在错误发生时调用，确保资源正确清理
     */
    void reset() {
        m_state = State::Invalid;
        m_send_awaitable.reset();
        m_recv_awaitable.reset();
        m_response = HttpResponse();  // 清空响应
    }

private:
    enum class State {
        Invalid,           // 无效状态，可以重新创建
        Sending,           // 正在发送请求
        Receiving          // 正在接收响应
    };

    HttpClient& m_client;
    HttpRequest m_request;
    HttpResponse m_response;
    State m_state;

    // 持有底层的 awaitable 对象
    std::optional<SendResponseAwaitable> m_send_awaitable;
    std::optional<GetResponseAwaitable> m_recv_awaitable;
};

using namespace galay::async;
using namespace galay::kernel;

/**
 * @brief HTTP客户端配置
 */
struct HttpClientConfig
{
    HttpReaderSetting reader_setting;
    HttpWriterSetting writer_setting;
    size_t ring_buffer_size = 8192;
};

/**
 * @brief HTTP客户端类
 * @details 提供异步HTTP客户端功能，采用两段式接口：sendRequest + getResponse
 */
class HttpClient
{
public:
    /**
     * @brief 构造函数
     * @param socket TcpSocket右值引用
     * @param config 客户端配置
     */
    HttpClient(TcpSocket&& socket, const HttpClientConfig& config = HttpClientConfig());

    /**
     * @brief 析构函数
     */
    ~HttpClient() = default;

    // 禁用拷贝
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // 禁用移动
    HttpClient(HttpClient&&) = delete;
    HttpClient& operator=(HttpClient&&) = delete;

    /**
     * @brief 发送GET请求
     * @param uri 请求URI
     * @param headers 可选的额外请求头
     * @return HttpClientAwaitable& 客户端等待体引用
     */
    HttpClientAwaitable& get(const std::string& uri,
                             const std::map<std::string, std::string>& headers = {});

    /**
     * @brief 发送POST请求
     * @param uri 请求URI
     * @param body 请求体
     * @param content_type Content-Type，默认为 application/x-www-form-urlencoded
     * @param headers 可选的额外请求头
     * @return HttpClientAwaitable& 客户端等待体引用
     */
    HttpClientAwaitable& post(const std::string& uri,
                              const std::string& body,
                              const std::string& content_type = "application/x-www-form-urlencoded",
                              const std::map<std::string, std::string>& headers = {});

    /**
     * @brief 发送PUT请求
     * @param uri 请求URI
     * @param body 请求体
     * @param content_type Content-Type
     * @param headers 可选的额外请求头
     * @return HttpClientAwaitable& 客户端等待体引用
     */
    HttpClientAwaitable& put(const std::string& uri,
                             const std::string& body,
                             const std::string& content_type = "application/json",
                             const std::map<std::string, std::string>& headers = {});

    /**
     * @brief 发送DELETE请求
     * @param uri 请求URI
     * @param headers 可选的额外请求头
     * @return HttpClientAwaitable& 客户端等待体引用
     */
    HttpClientAwaitable& del(const std::string& uri,
                             const std::map<std::string, std::string>& headers = {});

    /**
     * @brief 发送HTTP请求
     * @param request HttpRequest引用
     * @return SendResponseAwaitable 发送等待体
     */
    SendResponseAwaitable sendRequest(HttpRequest& request) {
        return m_writer.sendRequest(request);
    }

    /**
     * @brief 接收HTTP响应
     * @param response HttpResponse引用
     * @return GetResponseAwaitable 接收等待体
     */
    GetResponseAwaitable getResponse(HttpResponse& response) {
        return m_reader.getResponse(response);
    }

    /**
     * @brief 发送chunk数据
     * @param data 要发送的数据
     * @param is_last 是否是最后一个chunk
     * @return SendResponseAwaitable
     */
    SendResponseAwaitable sendChunk(const std::string& data, bool is_last = false) {
        return m_writer.sendChunk(data, is_last);
    }

    /**
     * @brief 获取HttpReader
     * @return HttpReader引用
     */
    HttpReader& getReader() {
        return m_reader;
    }

    /**
     * @brief 获取HttpWriter
     * @return HttpWriter引用
     */
    HttpWriter& getWriter() {
        return m_writer;
    }

    /**
     * @brief 关闭连接
     * @return CloseAwaitable 关闭等待体
     */
    CloseAwaitable close() {
        return m_socket.close();
    }

    /**
     * @brief 获取底层socket
     * @return TcpSocket引用
     */
    TcpSocket& socket() { return m_socket; }

private:
    TcpSocket m_socket;
    RingBuffer m_ring_buffer;
    HttpClientConfig m_config;
    HttpWriter m_writer;
    HttpReader m_reader;
    std::optional<HttpClientAwaitable> m_awaitable;  // 存储 awaitable 对象
};

} // namespace galay::http

#endif // GALAY_HTTP_CLIENT_H
