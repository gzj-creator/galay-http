#ifndef GALAY_HTTP_CLIENT_H
#define GALAY_HTTP_CLIENT_H

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
#include <regex>

namespace galay::websocket {
    template<typename SocketType>
    class WsConnImpl;
}

namespace galay::http
{

using namespace galay::async;
using namespace galay::kernel;

/**
 * @brief HTTP URL 解析结果
 */
struct HttpUrl {
    std::string scheme;
    std::string host;
    int port;
    std::string path;
    bool is_secure;

    static std::optional<HttpUrl> parse(const std::string& url) {
        std::regex url_regex(R"(^(http|https)://([^:/]+)(?::(\d+))?(/.*)?$)", std::regex::icase);
        std::smatch matches;

        if (!std::regex_match(url, matches, url_regex)) {
            HTTP_LOG_ERROR("Invalid HTTP URL format: {}", url);
            return std::nullopt;
        }

        HttpUrl result;
        result.scheme = matches[1].str();
        result.host = matches[2].str();
        result.is_secure = (result.scheme == "https" || result.scheme == "HTTPS");

        if (matches[3].matched) {
            try {
                result.port = std::stoi(matches[3].str());
            } catch (...) {
                HTTP_LOG_ERROR("Invalid port number in URL: {}", url);
                return std::nullopt;
            }
        } else {
            result.port = result.is_secure ? 443 : 80;
        }

        if (matches[4].matched) {
            result.path = matches[4].str();
        } else {
            result.path = "/";
        }

        return result;
    }
};

// 前向声明
template<typename SocketType>
class HttpClientImpl;

/**
 * @brief HTTP客户端等待体模板类
 */
template<typename SocketType>
class HttpClientAwaitableImpl : public galay::kernel::TimeoutSupport<HttpClientAwaitableImpl<SocketType>>
{
public:
    HttpClientAwaitableImpl(HttpClientImpl<SocketType>& client, HttpRequest&& request)
        : m_client(client)
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
            m_send_awaitable.emplace(m_client.getWriter().sendRequest(m_request));
            return m_send_awaitable->await_suspend(handle);
        } else if (m_state == State::Sending) {
            m_send_awaitable.emplace(m_client.getWriter().sendRequest(m_request));
            return m_send_awaitable->await_suspend(handle);
        } else {
            m_recv_awaitable.emplace(m_client.getReader().getResponse(m_response));
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
            return std::unexpected(HttpError(kInternalError, "HttpClientAwaitable in Invalid state"));
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

    HttpClientImpl<SocketType>& m_client;
    HttpRequest m_request;
    HttpResponse m_response;
    State m_state;

    std::optional<SendResponseAwaitableImpl<SocketType>> m_send_awaitable;
    std::optional<GetResponseAwaitableImpl<SocketType>> m_recv_awaitable;

public:
    std::expected<std::optional<HttpResponse>, galay::kernel::IOError> m_result;
};

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
 * @brief HTTP客户端模板类
 */
template<typename SocketType>
class HttpClientImpl
{
public:
    HttpClientImpl(const HttpClientConfig& config = HttpClientConfig())
        : m_socket(nullptr)
        , m_ring_buffer(nullptr)
        , m_config(config)
        , m_writer(nullptr)
        , m_reader(nullptr)
    {
    }

    HttpClientImpl(SocketType&& socket, const HttpClientConfig& config = HttpClientConfig())
        : m_socket(std::make_unique<SocketType>(std::move(socket)))
        , m_ring_buffer(std::make_unique<RingBuffer>(config.ring_buffer_size))
        , m_config(config)
        , m_writer(std::make_unique<HttpWriterImpl<SocketType>>(config.writer_setting, *m_socket))
        , m_reader(std::make_unique<HttpReaderImpl<SocketType>>(*m_ring_buffer, config.reader_setting, *m_socket))
    {
    }

    ~HttpClientImpl() = default;

    HttpClientImpl(const HttpClientImpl&) = delete;
    HttpClientImpl& operator=(const HttpClientImpl&) = delete;
    HttpClientImpl(HttpClientImpl&&) = delete;
    HttpClientImpl& operator=(HttpClientImpl&&) = delete;

    auto connect(const std::string& url) {
        auto parsed_url = HttpUrl::parse(url);
        if (!parsed_url) {
            throw std::runtime_error("Invalid HTTP URL: " + url);
        }

        m_url = parsed_url.value();

        if constexpr (std::is_same_v<SocketType, TcpSocket>) {
            if (m_url.is_secure) {
                throw std::runtime_error("HTTPS requires HttpsClient");
            }
        }

        HTTP_LOG_INFO("Connecting to server at {}:{}{}", m_url.host, m_url.port, m_url.path);

        m_socket = std::make_unique<SocketType>(IPType::IPV4);
        m_ring_buffer = std::make_unique<RingBuffer>(m_config.ring_buffer_size);

        auto nonblock_result = m_socket->option().handleNonBlock();
        if (!nonblock_result) {
            throw std::runtime_error("Failed to set non-blocking: " + nonblock_result.error().message());
        }

        m_writer = std::make_unique<HttpWriterImpl<SocketType>>(m_config.writer_setting, *m_socket);
        m_reader = std::make_unique<HttpReaderImpl<SocketType>>(*m_ring_buffer, m_config.reader_setting, *m_socket);

        Host server_host(IPType::IPV4, m_url.host, m_url.port);
        return m_socket->connect(server_host);
    }

    HttpClientAwaitableImpl<SocketType>& get(const std::string& uri,
                                              const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::GET, uri, "", "", headers);
    }

    HttpClientAwaitableImpl<SocketType>& post(const std::string& uri,
                                               const std::string& body,
                                               const std::string& content_type = "application/x-www-form-urlencoded",
                                               const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::POST, uri, body, content_type, headers);
    }

    HttpClientAwaitableImpl<SocketType>& put(const std::string& uri,
                                              const std::string& body,
                                              const std::string& content_type = "application/json",
                                              const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::PUT, uri, body, content_type, headers);
    }

    HttpClientAwaitableImpl<SocketType>& del(const std::string& uri,
                                              const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::DELETE, uri, "", "", headers);
    }

    HttpClientAwaitableImpl<SocketType>& head(const std::string& uri,
                                               const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::HEAD, uri, "", "", headers);
    }

    HttpClientAwaitableImpl<SocketType>& options(const std::string& uri,
                                                  const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::OPTIONS, uri, "", "", headers);
    }

    HttpClientAwaitableImpl<SocketType>& patch(const std::string& uri,
                                                const std::string& body,
                                                const std::string& content_type = "application/json",
                                                const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::PATCH, uri, body, content_type, headers);
    }

    HttpClientAwaitableImpl<SocketType>& trace(const std::string& uri,
                                                const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::TRACE, uri, "", "", headers);
    }

    HttpClientAwaitableImpl<SocketType>& tunnel(const std::string& target_host,
                                                 const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::CONNECT, target_host, "", "", headers);
    }

    SendResponseAwaitableImpl<SocketType> sendRequest(HttpRequest& request) {
        return m_writer->sendRequest(request);
    }

    GetResponseAwaitableImpl<SocketType> getResponse(HttpResponse& response) {
        return m_reader->getResponse(response);
    }

    SendResponseAwaitableImpl<SocketType> sendChunk(const std::string& data, bool is_last = false) {
        return m_writer->sendChunk(data, is_last);
    }

    HttpReaderImpl<SocketType>& getReader() {
        return *m_reader;
    }

    HttpWriterImpl<SocketType>& getWriter() {
        return *m_writer;
    }

    auto close() {
        return m_socket->close();
    }

    SocketType& socket() { return *m_socket; }
    RingBuffer& ringBuffer() { return *m_ring_buffer; }
    const HttpUrl& url() const { return m_url; }

private:
    HttpClientAwaitableImpl<SocketType>& createRequest(HttpMethod method,
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

    std::unique_ptr<SocketType> m_socket;
    std::unique_ptr<RingBuffer> m_ring_buffer;
    HttpClientConfig m_config;
    std::unique_ptr<HttpWriterImpl<SocketType>> m_writer;
    std::unique_ptr<HttpReaderImpl<SocketType>> m_reader;
    std::optional<HttpClientAwaitableImpl<SocketType>> m_awaitable;
    HttpUrl m_url;
};

// 类型别名 - HTTP (TcpSocket)
using HttpClientAwaitable = HttpClientAwaitableImpl<TcpSocket>;
using HttpClient = HttpClientImpl<TcpSocket>;

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-socket/async/SslSocket.h"
using HttpsClientAwaitable = HttpClientAwaitableImpl<galay::async::SslSocket>;
using HttpsClient = HttpClientImpl<galay::async::SslSocket>;
#endif

} // namespace galay::http

#endif // GALAY_HTTP_CLIENT_H
