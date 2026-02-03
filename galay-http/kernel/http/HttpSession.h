#ifndef GALAY_HTTP_SESSION_H
#define GALAY_HTTP_SESSION_H

#include "HttpWriter.h"
#include "HttpReader.h"
#include "HttpLog.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/common/Buffer.h"
#include "galay-kernel/kernel/Timeout.hpp"
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
template<typename SocketType>
class HttpSessionImpl;

/**
 * @brief HTTP会话等待体模板类
 */
template<typename SocketType>
class HttpSessionAwaitableImpl : public galay::kernel::TimeoutSupport<HttpSessionAwaitableImpl<SocketType>>
{
public:
    HttpSessionAwaitableImpl(HttpSessionImpl<SocketType>& session, HttpRequest&& request)
        : m_session(session)
        , m_request(std::move(request))
        , m_response()
        , m_state(State::Invalid)
        , m_send_awaitable(std::nullopt)
        , m_recv_awaitable(std::nullopt)
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    bool await_suspend(std::coroutine_handle<> handle) {
        if (m_state == State::Invalid) {
            m_state = State::Sending;
            m_send_awaitable.emplace(m_session.getWriter().sendRequest(m_request));
            return m_send_awaitable->await_suspend(handle);
        } else if (m_state == State::Sending) {
            m_send_awaitable.emplace(m_session.getWriter().sendRequest(m_request));
            return m_send_awaitable->await_suspend(handle);
        } else {
            m_recv_awaitable.emplace(m_session.getReader().getResponse(m_response));
            return m_recv_awaitable->await_suspend(handle);
        }
    }

    std::expected<std::optional<HttpResponse>, HttpError> await_resume() {
        if (!m_result.has_value()) {
            auto& io_error = m_result.error();
            HTTP_LOG_DEBUG("request failed with IO error: {}", io_error.message());

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
            auto sendResult = m_send_awaitable->await_resume();

            if (!sendResult) {
                HTTP_LOG_DEBUG("send request failed: {}", sendResult.error().message());
                reset();
                return std::unexpected(sendResult.error());
            }

            if (!sendResult.value()) {
                return std::nullopt;
            }

            m_state = State::Receiving;
            m_send_awaitable.reset();
            return std::nullopt;
        } else if (m_state == State::Receiving) {
            auto recvResult = m_recv_awaitable->await_resume();

            if (!recvResult) {
                HTTP_LOG_DEBUG("receive response failed: {}", recvResult.error().message());
                reset();
                return std::unexpected(recvResult.error());
            }

            if (!recvResult.value()) {
                return std::nullopt;
            }

            auto response = std::move(m_response);
            reset();
            return response;
        } else {
            HTTP_LOG_ERROR("await_resume called in Invalid state");
            reset();
            return std::unexpected(HttpError(kInternalError, "HttpSessionAwaitable in Invalid state"));
        }
    }

    bool isInvalid() const {
        return m_state == State::Invalid;
    }

    void reset() {
        m_state = State::Invalid;
        m_send_awaitable.reset();
        m_recv_awaitable.reset();
        m_response = HttpResponse();
        m_result = std::nullopt;
    }

private:
    enum class State {
        Invalid,
        Sending,
        Receiving
    };

    HttpSessionImpl<SocketType>& m_session;
    HttpRequest m_request;
    HttpResponse m_response;
    State m_state;

    std::optional<SendResponseAwaitableImpl<SocketType>> m_send_awaitable;
    std::optional<GetResponseAwaitableImpl<SocketType>> m_recv_awaitable;

public:
    std::expected<std::optional<HttpResponse>, galay::kernel::IOError> m_result;
};

/**
 * @brief HTTP会话模板类
 * @details 持有 socket、ring_buffer、reader 和 writer，负责实际的 HTTP 通信
 */
template<typename SocketType>
class HttpSessionImpl
{
public:
    HttpSessionImpl(SocketType& socket,
                    size_t ring_buffer_size = 8192,
                    const HttpReaderSetting& reader_setting = HttpReaderSetting(),
                    const HttpWriterSetting& writer_setting = HttpWriterSetting())
        : m_socket(socket)
        , m_ring_buffer(ring_buffer_size)
        , m_reader(m_ring_buffer, reader_setting, socket)
        , m_writer(writer_setting, socket)
    {
    }

    HttpReaderImpl<SocketType>& getReader() {
        return m_reader;
    }

    HttpWriterImpl<SocketType>& getWriter() {
        return m_writer;
    }

    // 便捷方法：GET 请求
    HttpSessionAwaitableImpl<SocketType>& get(const std::string& uri,
                                               const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::GET, uri, "", "", headers);
    }

    // 便捷方法：POST 请求
    HttpSessionAwaitableImpl<SocketType>& post(const std::string& uri,
                                                const std::string& body,
                                                const std::string& content_type = "application/x-www-form-urlencoded",
                                                const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::POST, uri, body, content_type, headers);
    }

    // 便捷方法：PUT 请求
    HttpSessionAwaitableImpl<SocketType>& put(const std::string& uri,
                                               const std::string& body,
                                               const std::string& content_type = "application/json",
                                               const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::PUT, uri, body, content_type, headers);
    }

    // 便捷方法：DELETE 请求
    HttpSessionAwaitableImpl<SocketType>& del(const std::string& uri,
                                               const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::DELETE, uri, "", "", headers);
    }

    // 便捷方法：HEAD 请求
    HttpSessionAwaitableImpl<SocketType>& head(const std::string& uri,
                                                const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::HEAD, uri, "", "", headers);
    }

    // 便捷方法：OPTIONS 请求
    HttpSessionAwaitableImpl<SocketType>& options(const std::string& uri,
                                                   const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::OPTIONS, uri, "", "", headers);
    }

    // 便捷方法：PATCH 请求
    HttpSessionAwaitableImpl<SocketType>& patch(const std::string& uri,
                                                 const std::string& body,
                                                 const std::string& content_type = "application/json",
                                                 const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::PATCH, uri, body, content_type, headers);
    }

    // 便捷方法：TRACE 请求
    HttpSessionAwaitableImpl<SocketType>& trace(const std::string& uri,
                                                 const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::TRACE, uri, "", "", headers);
    }

    // 便捷方法：CONNECT 请求（用于隧道）
    HttpSessionAwaitableImpl<SocketType>& tunnel(const std::string& target_host,
                                                  const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::CONNECT, target_host, "", "", headers);
    }

    // 发送请求
    SendResponseAwaitableImpl<SocketType> sendRequest(HttpRequest& request) {
        return m_writer.sendRequest(request);
    }

    // 接收响应
    GetResponseAwaitableImpl<SocketType> getResponse(HttpResponse& response) {
        return m_reader.getResponse(response);
    }

    // 发送分块数据
    SendResponseAwaitableImpl<SocketType> sendChunk(const std::string& data, bool is_last = false) {
        return m_writer.sendChunk(data, is_last);
    }

private:
    HttpSessionAwaitableImpl<SocketType>& createRequest(HttpMethod method,
                                                         const std::string& uri,
                                                         const std::string& body,
                                                         const std::string& content_type,
                                                         const std::map<std::string, std::string>& headers) {
        if (!m_awaitable.has_value() || m_awaitable->isInvalid()) {
            HttpRequest request;
            HttpRequestHeader header;

            header.method() = method;
            header.uri() = uri;
            header.version() = HttpVersion::HttpVersion_1_1;

            if (!body.empty() && !content_type.empty()) {
                header.headerPairs().addHeaderPair("Content-Type", content_type);
                header.headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));
            }

            for (const auto& [key, value] : headers) {
                header.headerPairs().addHeaderPair(key, value);
            }

            request.setHeader(std::move(header));

            if (!body.empty()) {
                std::string body_copy = body;
                request.setBodyStr(std::move(body_copy));
            }

            m_awaitable.emplace(*this, std::move(request));
        }

        return *m_awaitable;
    }

    SocketType& m_socket;
    RingBuffer m_ring_buffer;
    HttpReaderImpl<SocketType> m_reader;
    HttpWriterImpl<SocketType> m_writer;
    std::optional<HttpSessionAwaitableImpl<SocketType>> m_awaitable;
};

// 类型别名 - HTTP (TcpSocket)
using HttpSessionAwaitable = HttpSessionAwaitableImpl<TcpSocket>;
using HttpSession = HttpSessionImpl<TcpSocket>;

} // namespace galay::http

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/SslSocket.h"

namespace galay::http {

// 类型别名 - HTTPS (SslSocket)
using HttpsSessionAwaitable = HttpSessionAwaitableImpl<galay::ssl::SslSocket>;
using HttpsSession = HttpSessionImpl<galay::ssl::SslSocket>;

} // namespace galay::http
#endif

#endif // GALAY_HTTP_SESSION_H
