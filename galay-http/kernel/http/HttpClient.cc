#include "HttpClient.h"
#include "HttpLog.h"

namespace galay::http
{

// ==================== HttpClientAwaitable 实现 ====================

HttpClientAwaitable::HttpClientAwaitable(HttpClient& client, HttpRequest&& request)
    : m_client(client)
    , m_request(std::move(request))
    , m_response()
    , m_state(State::Invalid)
    , m_send_awaitable(std::nullopt)
    , m_recv_awaitable(std::nullopt)
{
}

bool HttpClientAwaitable::await_suspend(std::coroutine_handle<> handle)
{
    if (m_state == State::Invalid) {
        // Invalid 状态，开始发送请求
        m_state = State::Sending;
        m_send_awaitable.emplace(m_client.getWriter().sendRequest(m_request));
        return m_send_awaitable->await_suspend(handle);
    } else if (m_state == State::Sending) {
        // 继续发送请求（重新创建 awaitable）
        m_send_awaitable.emplace(m_client.getWriter().sendRequest(m_request));
        return m_send_awaitable->await_suspend(handle);
    } else {
        // Receiving 状态，接收响应（重新创建 awaitable）
        m_recv_awaitable.emplace(m_client.getReader().getResponse(m_response));
        return m_recv_awaitable->await_suspend(handle);
    }
}

std::expected<std::optional<HttpResponse>, HttpError> HttpClientAwaitable::await_resume()
{
    // 首先检查是否有超时错误（由 TimeoutSupport 设置）
    if (!m_result.has_value()) {
        // m_result 已被 TimeoutSupport 设置为 IOError，需要转换为 HttpError
        auto& io_error = m_result.error();
        HTTP_LOG_DEBUG("request failed with IO error: {}", io_error.message());

        // 将 IOError 转换为 HttpError
        HttpErrorCode http_error_code;
        if (io_error.code() == kTimeout) {
            http_error_code = kRequestTimeOut;
        } else if (io_error.code() == kDisconnectError) {
            http_error_code = kConnectionClose;
        } else {
            http_error_code = kTcpRecvError;
        }

        reset();
        return std::unexpected(HttpError(http_error_code, io_error.message()));
    }

    if (m_state == State::Sending) {
        // 检查发送结果
        auto sendResult = m_send_awaitable->await_resume();

        if (!sendResult) {
            // 发送错误，清理资源并重置为 Invalid 状态
            HTTP_LOG_DEBUG("send request failed: {}", sendResult.error().message());
            reset();
            return std::unexpected(sendResult.error());
        }

        if (!sendResult.value()) {
            // 发送未完成，保持 Sending 状态
            return std::nullopt;
        }

        // 发送完成，立刻切换到 Receiving 状态
        m_state = State::Receiving;
        m_send_awaitable.reset();  // 清理发送 awaitable
        return std::nullopt;
    } else if (m_state == State::Receiving) {
        // Receiving 状态，检查接收结果
        auto recvResult = m_recv_awaitable->await_resume();

        if (!recvResult) {
            // 接收错误，清理资源并重置为 Invalid 状态
            HTTP_LOG_DEBUG("receive response failed: {}", recvResult.error().message());
            reset();
            return std::unexpected(recvResult.error());
        }

        if (!recvResult.value()) {
            // 接收未完成，保持 Receiving 状态
            return std::nullopt;
        }

        // 接收完成，立刻重置为 Invalid 状态
        auto response = std::move(m_response);
        reset();  // 清理所有资源
        return response;
    } else {
        // Invalid 状态，不应该被调用
        HTTP_LOG_ERROR("await_resume called in Invalid state");
        reset();
        return std::unexpected(HttpError(kInternalError, "HttpClientAwaitable in Invalid state"));
    }
}

// ==================== HttpClient 实现 ====================

HttpClient::HttpClient(TcpSocket&& socket, const HttpClientConfig& config)
    : m_socket(std::move(socket))
    , m_ring_buffer(config.ring_buffer_size)
    , m_config(config)
    , m_writer(config.writer_setting, m_socket)
    , m_reader(m_ring_buffer, config.reader_setting, m_socket)
{
}

HttpClientAwaitable& HttpClient::get(const std::string& uri,
                                     const std::map<std::string, std::string>& headers)
{
    // 只有当 awaitable 不存在或状态为 Invalid 时，才创建新的
    if (!m_awaitable.has_value() || m_awaitable->isInvalid()) {
        HttpRequest request;
        HttpRequestHeader header;

        header.method() = HttpMethod::GET;
        header.uri() = uri;
        header.version() = HttpVersion::HttpVersion_1_1;

        // 添加额外的请求头
        for (const auto& [key, value] : headers) {
            header.headerPairs().addHeaderPair(key, value);
        }

        request.setHeader(std::move(header));

        // 在 HttpClient 内部创建并存储 awaitable
        m_awaitable.emplace(*this, std::move(request));
    }

    return *m_awaitable;
}

HttpClientAwaitable& HttpClient::post(const std::string& uri,
                                      const std::string& body,
                                      const std::string& content_type,
                                      const std::map<std::string, std::string>& headers)
{
    // 只有当 awaitable 不存在或状态为 Invalid 时，才创建新的
    if (!m_awaitable.has_value() || m_awaitable->isInvalid()) {
        HttpRequest request;
        HttpRequestHeader header;

        header.method() = HttpMethod::POST;
        header.uri() = uri;
        header.version() = HttpVersion::HttpVersion_1_1;
        header.headerPairs().addHeaderPair("Content-Type", content_type);
        header.headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));

        // 添加额外的请求头
        for (const auto& [key, value] : headers) {
            header.headerPairs().addHeaderPair(key, value);
        }

        request.setHeader(std::move(header));

        // 创建 body 的可修改副本
        std::string body_copy = body;
        request.setBodyStr(std::move(body_copy));

        // 在 HttpClient 内部创建并存储 awaitable
        m_awaitable.emplace(*this, std::move(request));
    }

    return *m_awaitable;
}

HttpClientAwaitable& HttpClient::put(const std::string& uri,
                                     const std::string& body,
                                     const std::string& content_type,
                                     const std::map<std::string, std::string>& headers)
{
    // 只有当 awaitable 不存在或状态为 Invalid 时，才创建新的
    if (!m_awaitable.has_value() || m_awaitable->isInvalid()) {
        HttpRequest request;
        HttpRequestHeader header;

        header.method() = HttpMethod::PUT;
        header.uri() = uri;
        header.version() = HttpVersion::HttpVersion_1_1;
        header.headerPairs().addHeaderPair("Content-Type", content_type);
        header.headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));

        // 添加额外的请求头
        for (const auto& [key, value] : headers) {
            header.headerPairs().addHeaderPair(key, value);
        }

        request.setHeader(std::move(header));

        // 创建 body 的可修改副本
        std::string body_copy = body;
        request.setBodyStr(std::move(body_copy));

        // 在 HttpClient 内部创建并存储 awaitable
        m_awaitable.emplace(*this, std::move(request));
    }

    return *m_awaitable;
}

HttpClientAwaitable& HttpClient::del(const std::string& uri,
                                     const std::map<std::string, std::string>& headers)
{
    // 只有当 awaitable 不存在或状态为 Invalid 时，才创建新的
    if (!m_awaitable.has_value() || m_awaitable->isInvalid()) {
        HttpRequest request;
        HttpRequestHeader header;

        header.method() = HttpMethod::DELETE;
        header.uri() = uri;
        header.version() = HttpVersion::HttpVersion_1_1;

        // 添加额外的请求头
        for (const auto& [key, value] : headers) {
            header.headerPairs().addHeaderPair(key, value);
        }

        request.setHeader(std::move(header));

        // 在 HttpClient 内部创建并存储 awaitable
        m_awaitable.emplace(*this, std::move(request));
    }

    return *m_awaitable;
}

HttpClientAwaitable& HttpClient::head(const std::string& uri,
                                      const std::map<std::string, std::string>& headers)
{
    // 只有当 awaitable 不存在或状态为 Invalid 时，才创建新的
    if (!m_awaitable.has_value() || m_awaitable->isInvalid()) {
        HttpRequest request;
        HttpRequestHeader header;

        header.method() = HttpMethod::HEAD;
        header.uri() = uri;
        header.version() = HttpVersion::HttpVersion_1_1;

        // 添加额外的请求头
        for (const auto& [key, value] : headers) {
            header.headerPairs().addHeaderPair(key, value);
        }

        request.setHeader(std::move(header));

        // 在 HttpClient 内部创建并存储 awaitable
        m_awaitable.emplace(*this, std::move(request));
    }

    return *m_awaitable;
}

HttpClientAwaitable& HttpClient::options(const std::string& uri,
                                         const std::map<std::string, std::string>& headers)
{
    // 只有当 awaitable 不存在或状态为 Invalid 时，才创建新的
    if (!m_awaitable.has_value() || m_awaitable->isInvalid()) {
        HttpRequest request;
        HttpRequestHeader header;

        header.method() = HttpMethod::OPTIONS;
        header.uri() = uri;
        header.version() = HttpVersion::HttpVersion_1_1;

        // 添加额外的请求头
        for (const auto& [key, value] : headers) {
            header.headerPairs().addHeaderPair(key, value);
        }

        request.setHeader(std::move(header));

        // 在 HttpClient 内部创建并存储 awaitable
        m_awaitable.emplace(*this, std::move(request));
    }

    return *m_awaitable;
}

HttpClientAwaitable& HttpClient::patch(const std::string& uri,
                                       const std::string& body,
                                       const std::string& content_type,
                                       const std::map<std::string, std::string>& headers)
{
    // 只有当 awaitable 不存在或状态为 Invalid 时，才创建新的
    if (!m_awaitable.has_value() || m_awaitable->isInvalid()) {
        HttpRequest request;
        HttpRequestHeader header;

        header.method() = HttpMethod::PATCH;
        header.uri() = uri;
        header.version() = HttpVersion::HttpVersion_1_1;
        header.headerPairs().addHeaderPair("Content-Type", content_type);
        header.headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));

        // 添加额外的请求头
        for (const auto& [key, value] : headers) {
            header.headerPairs().addHeaderPair(key, value);
        }

        request.setHeader(std::move(header));

        // 创建 body 的可修改副本
        std::string body_copy = body;
        request.setBodyStr(std::move(body_copy));

        // 在 HttpClient 内部创建并存储 awaitable
        m_awaitable.emplace(*this, std::move(request));
    }

    return *m_awaitable;
}

HttpClientAwaitable& HttpClient::trace(const std::string& uri,
                                       const std::map<std::string, std::string>& headers)
{
    // 只有当 awaitable 不存在或状态为 Invalid 时，才创建新的
    if (!m_awaitable.has_value() || m_awaitable->isInvalid()) {
        HttpRequest request;
        HttpRequestHeader header;

        header.method() = HttpMethod::TRACE;
        header.uri() = uri;
        header.version() = HttpVersion::HttpVersion_1_1;

        // 添加额外的请求头
        for (const auto& [key, value] : headers) {
            header.headerPairs().addHeaderPair(key, value);
        }

        request.setHeader(std::move(header));

        // 在 HttpClient 内部创建并存储 awaitable
        m_awaitable.emplace(*this, std::move(request));
    }

    return *m_awaitable;
}

HttpClientAwaitable& HttpClient::connect(const std::string& uri,
                                         const std::map<std::string, std::string>& headers)
{
    // 只有当 awaitable 不存在或状态为 Invalid 时，才创建新的
    if (!m_awaitable.has_value() || m_awaitable->isInvalid()) {
        HttpRequest request;
        HttpRequestHeader header;

        header.method() = HttpMethod::CONNECT;
        header.uri() = uri;
        header.version() = HttpVersion::HttpVersion_1_1;

        // 添加额外的请求头
        for (const auto& [key, value] : headers) {
            header.headerPairs().addHeaderPair(key, value);
        }

        request.setHeader(std::move(header));

        // 在 HttpClient 内部创建并存储 awaitable
        m_awaitable.emplace(*this, std::move(request));
    }

    return *m_awaitable;
}

} // namespace galay::http
