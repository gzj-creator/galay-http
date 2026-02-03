#ifndef GALAY_HTTP2_WRITER_H
#define GALAY_HTTP2_WRITER_H

#include "Http2Conn.h"
#include "Http2Stream.h"
#include "galay-http/protoc/http2/Http2Base.h"
#include "galay-http/protoc/http2/Http2Frame.h"
#include "galay-http/protoc/http2/Http2Error.h"
#include "galay-kernel/kernel/Timeout.hpp"
#include <expected>

namespace galay::http2
{

using namespace galay::kernel;

// 前向声明
template<typename SocketType>
class Http2WriterImpl;

/**
 * @brief HTTP/2 发送 HEADERS 帧等待体
 */
template<typename SocketType>
class SendHeadersAwaitableImpl : public TimeoutSupport<SendHeadersAwaitableImpl<SocketType>>
{
public:
    SendHeadersAwaitableImpl(Http2WriterImpl<SocketType>& writer, uint32_t stream_id,
                            std::vector<Http2HeaderField> headers, bool end_stream)
        : m_writer(&writer)
        , m_stream_id(stream_id)
        , m_headers(std::move(headers))
        , m_end_stream(end_stream)
    {
    }

    bool await_ready() const noexcept { return false; }

    template<typename Handle>
    auto await_suspend(Handle handle) {
        if (!m_send_awaitable.has_value()) {
            m_send_awaitable.emplace(m_writer->m_conn->sendHeaders(m_stream_id, m_headers, m_end_stream, true));
        }
        return m_send_awaitable->await_suspend(handle);
    }

    std::expected<bool, Http2ErrorCode> await_resume() {
        auto result = m_send_awaitable->await_resume();
        m_send_awaitable.reset();

        if (!result) {
            return std::unexpected(Http2ErrorCode::InternalError);
        }

        return result.value();
    }

private:
    Http2WriterImpl<SocketType>* m_writer;
    uint32_t m_stream_id;
    std::vector<Http2HeaderField> m_headers;
    bool m_end_stream;
    std::optional<decltype(std::declval<Http2ConnImpl<SocketType>>().sendHeaders(0, m_headers, false, true))> m_send_awaitable;

public:
    std::optional<std::expected<void, IOError>> m_result;
};

/**
 * @brief HTTP/2 发送 DATA 帧等待体
 */
template<typename SocketType>
class SendDataAwaitableImpl : public TimeoutSupport<SendDataAwaitableImpl<SocketType>>
{
public:
    SendDataAwaitableImpl(Http2WriterImpl<SocketType>& writer, uint32_t stream_id,
                         std::string data, bool end_stream)
        : m_writer(&writer)
        , m_stream_id(stream_id)
        , m_data(std::move(data))
        , m_end_stream(end_stream)
    {
    }

    bool await_ready() const noexcept { return false; }

    template<typename Handle>
    auto await_suspend(Handle handle) {
        if (!m_send_awaitable.has_value()) {
            m_send_awaitable.emplace(m_writer->m_conn->sendDataFrame(m_stream_id, m_data, m_end_stream));
        }
        return m_send_awaitable->await_suspend(handle);
    }

    std::expected<bool, Http2ErrorCode> await_resume() {
        auto result = m_send_awaitable->await_resume();
        m_send_awaitable.reset();

        if (!result) {
            return std::unexpected(Http2ErrorCode::InternalError);
        }

        return result.value();
    }

private:
    Http2WriterImpl<SocketType>* m_writer;
    uint32_t m_stream_id;
    std::string m_data;
    bool m_end_stream;
    std::optional<decltype(std::declval<Http2ConnImpl<SocketType>>().sendDataFrame(0, m_data, false))> m_send_awaitable;

public:
    std::optional<std::expected<void, IOError>> m_result;
};

/**
 * @brief HTTP/2 发送请求等待体（用于客户端）
 */
template<typename SocketType>
class SendRequestAwaitableImpl : public TimeoutSupport<SendRequestAwaitableImpl<SocketType>>
{
public:
    SendRequestAwaitableImpl(Http2WriterImpl<SocketType>& writer, uint32_t stream_id, Http2Request request)
        : m_writer(&writer)
        , m_stream_id(stream_id)
        , m_request(std::move(request))
        , m_state(State::SendingHeaders)
    {
    }

    bool await_ready() const noexcept { return false; }

    template<typename Handle>
    auto await_suspend(Handle handle) {
        switch (m_state) {
            case State::SendingHeaders:
                if (!m_headers_awaitable.has_value()) {
                    std::vector<Http2HeaderField> headers;
                    headers.push_back({":method", m_request.method});
                    headers.push_back({":scheme", m_request.scheme});
                    headers.push_back({":authority", m_request.authority});
                    headers.push_back({":path", m_request.path.empty() ? "/" : m_request.path});
                    for (const auto& h : m_request.headers) {
                        headers.push_back(h);
                    }
                    bool end_stream = m_request.body.empty();
                    m_headers_awaitable.emplace(m_writer->sendHeaders(m_stream_id, std::move(headers), end_stream));
                }
                return m_headers_awaitable->await_suspend(handle);

            case State::SendingData:
                if (!m_data_awaitable.has_value()) {
                    m_data_awaitable.emplace(m_writer->sendData(m_stream_id, m_request.body, true));
                }
                return m_data_awaitable->await_suspend(handle);

            default:
                return false;
        }
    }

    std::expected<bool, Http2ErrorCode> await_resume() {
        switch (m_state) {
            case State::SendingHeaders: {
                auto result = m_headers_awaitable->await_resume();
                m_headers_awaitable.reset();

                if (!result) {
                    return std::unexpected(result.error());
                }

                if (!result.value()) {
                    return false;  // 需要继续发送
                }

                if (m_request.body.empty()) {
                    m_state = State::Done;
                    return true;  // 请求发送完成
                }

                m_state = State::SendingData;
                return false;  // 继续发送 DATA
            }

            case State::SendingData: {
                auto result = m_data_awaitable->await_resume();
                m_data_awaitable.reset();

                if (!result) {
                    return std::unexpected(result.error());
                }

                if (!result.value()) {
                    return false;  // 需要继续发送
                }

                m_state = State::Done;
                return true;  // 请求发送完成
            }

            default:
                return std::unexpected(Http2ErrorCode::InternalError);
        }
    }

private:
    enum class State { SendingHeaders, SendingData, Done };

    Http2WriterImpl<SocketType>* m_writer;
    uint32_t m_stream_id;
    Http2Request m_request;
    State m_state;

    std::optional<SendHeadersAwaitableImpl<SocketType>> m_headers_awaitable;
    std::optional<SendDataAwaitableImpl<SocketType>> m_data_awaitable;

public:
    std::optional<std::expected<void, IOError>> m_result;
};

/**
 * @brief HTTP/2 发送响应等待体（用于服务器）
 */
template<typename SocketType>
class SendResponseAwaitableImpl : public TimeoutSupport<SendResponseAwaitableImpl<SocketType>>
{
public:
    SendResponseAwaitableImpl(Http2WriterImpl<SocketType>& writer, uint32_t stream_id, Http2Response response)
        : m_writer(&writer)
        , m_stream_id(stream_id)
        , m_response(std::move(response))
        , m_state(State::SendingHeaders)
    {
    }

    bool await_ready() const noexcept { return false; }

    template<typename Handle>
    auto await_suspend(Handle handle) {
        switch (m_state) {
            case State::SendingHeaders:
                if (!m_headers_awaitable.has_value()) {
                    std::vector<Http2HeaderField> headers;
                    headers.push_back({":status", std::to_string(m_response.status)});
                    for (const auto& h : m_response.headers) {
                        headers.push_back(h);
                    }
                    bool end_stream = m_response.body.empty();
                    m_headers_awaitable.emplace(m_writer->sendHeaders(m_stream_id, std::move(headers), end_stream));
                }
                return m_headers_awaitable->await_suspend(handle);

            case State::SendingData:
                if (!m_data_awaitable.has_value()) {
                    m_data_awaitable.emplace(m_writer->sendData(m_stream_id, m_response.body, true));
                }
                return m_data_awaitable->await_suspend(handle);

            default:
                return false;
        }
    }

    std::expected<bool, Http2ErrorCode> await_resume() {
        switch (m_state) {
            case State::SendingHeaders: {
                auto result = m_headers_awaitable->await_resume();
                m_headers_awaitable.reset();

                if (!result) {
                    return std::unexpected(result.error());
                }

                if (!result.value()) {
                    return false;  // 需要继续发送
                }

                if (m_response.body.empty()) {
                    m_state = State::Done;
                    return true;  // 响应发送完成
                }

                m_state = State::SendingData;
                return false;  // 继续发送 DATA
            }

            case State::SendingData: {
                auto result = m_data_awaitable->await_resume();
                m_data_awaitable.reset();

                if (!result) {
                    return std::unexpected(result.error());
                }

                if (!result.value()) {
                    return false;  // 需要继续发送
                }

                m_state = State::Done;
                return true;  // 响应发送完成
            }

            default:
                return std::unexpected(Http2ErrorCode::InternalError);
        }
    }

private:
    enum class State { SendingHeaders, SendingData, Done };

    Http2WriterImpl<SocketType>* m_writer;
    uint32_t m_stream_id;
    Http2Response m_response;
    State m_state;

    std::optional<SendHeadersAwaitableImpl<SocketType>> m_headers_awaitable;
    std::optional<SendDataAwaitableImpl<SocketType>> m_data_awaitable;

public:
    std::optional<std::expected<void, IOError>> m_result;
};

/**
 * @brief HTTP/2 Writer
 */
template<typename SocketType>
class Http2WriterImpl
{
    friend class SendHeadersAwaitableImpl<SocketType>;
    friend class SendDataAwaitableImpl<SocketType>;
    friend class SendRequestAwaitableImpl<SocketType>;
    friend class SendResponseAwaitableImpl<SocketType>;

public:
    Http2WriterImpl(Http2ConnImpl<SocketType>& conn)
        : m_conn(&conn)
    {
    }

    /**
     * @brief 发送 HEADERS 帧
     */
    SendHeadersAwaitableImpl<SocketType> sendHeaders(uint32_t stream_id, std::vector<Http2HeaderField> headers, bool end_stream = false) {
        return SendHeadersAwaitableImpl<SocketType>(*this, stream_id, std::move(headers), end_stream);
    }

    /**
     * @brief 发送 DATA 帧
     */
    SendDataAwaitableImpl<SocketType> sendData(uint32_t stream_id, std::string data, bool end_stream = false) {
        return SendDataAwaitableImpl<SocketType>(*this, stream_id, std::move(data), end_stream);
    }

    /**
     * @brief 发送完整的 HTTP/2 请求（用于客户端）
     */
    SendRequestAwaitableImpl<SocketType> sendRequest(uint32_t stream_id, Http2Request request) {
        return SendRequestAwaitableImpl<SocketType>(*this, stream_id, std::move(request));
    }

    /**
     * @brief 发送完整的 HTTP/2 响应（用于服务器）
     */
    SendResponseAwaitableImpl<SocketType> sendResponse(uint32_t stream_id, Http2Response response) {
        return SendResponseAwaitableImpl<SocketType>(*this, stream_id, std::move(response));
    }

private:
    Http2ConnImpl<SocketType>* m_conn;
};

// 类型别名
using Http2Writer = Http2WriterImpl<galay::async::TcpSocket>;

#ifdef GALAY_HTTP_SSL_ENABLED
using Http2SslWriter = Http2WriterImpl<galay::ssl::SslSocket>;
#endif

} // namespace galay::http2

#endif // GALAY_HTTP2_WRITER_H
