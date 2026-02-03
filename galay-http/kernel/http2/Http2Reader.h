#ifndef GALAY_HTTP2_READER_H
#define GALAY_HTTP2_READER_H

#include "Http2Conn.h"
#include "Http2Stream.h"
#include "galay-http/protoc/http2/Http2Base.h"
#include "galay-http/protoc/http2/Http2Frame.h"
#include "galay-http/protoc/http2/Http2Error.h"
#include "galay-kernel/kernel/Timeout.hpp"
#include <expected>
#include <optional>

namespace galay::http2
{

using namespace galay::kernel;

// 前向声明
template<typename SocketType>
class Http2ReaderImpl;

/**
 * @brief HTTP/2 帧读取等待体
 */
template<typename SocketType>
class GetFrameAwaitableImpl : public TimeoutSupport<GetFrameAwaitableImpl<SocketType>>
{
public:
    GetFrameAwaitableImpl(Http2ReaderImpl<SocketType>& reader)
        : m_reader(&reader)
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    template<typename Handle>
    auto await_suspend(Handle handle) {
        if (!m_read_awaitable.has_value()) {
            m_read_awaitable.emplace(m_reader->m_conn.readFrame());
        }
        return m_read_awaitable->await_suspend(handle);
    }

    std::expected<std::optional<Http2Frame::uptr>, Http2ErrorCode> await_resume() {
        auto result = m_read_awaitable->await_resume();
        m_read_awaitable.reset();

        if (!result) {
            // NoError 表示需要更多数据
            if (result.error() == Http2ErrorCode::NoError) {
                return std::nullopt;
            }
            return std::unexpected(result.error());
        }

        return std::move(*result);
    }

private:
    Http2ReaderImpl<SocketType>* m_reader;
    std::optional<Http2ReadFrameAwaitableImpl<SocketType>> m_read_awaitable;

public:
    std::optional<std::expected<void, IOError>> m_result;
};

/**
 * @brief HTTP/2 读取响应等待体（用于客户端）
 */
template<typename SocketType>
class GetResponseAwaitableImpl : public TimeoutSupport<GetResponseAwaitableImpl<SocketType>>
{
public:
    GetResponseAwaitableImpl(Http2ReaderImpl<SocketType>& reader, uint32_t stream_id, Http2Response& response)
        : m_reader(&reader)
        , m_stream_id(stream_id)
        , m_response(&response)
    {
    }

    bool await_ready() const noexcept { return false; }

    template<typename Handle>
    auto await_suspend(Handle handle) {
        if (!m_frame_awaitable.has_value()) {
            m_frame_awaitable.emplace(m_reader->getFrame());
        }
        return m_frame_awaitable->await_suspend(handle);
    }

    std::expected<bool, Http2ErrorCode> await_resume() {
        auto result = m_frame_awaitable->await_resume();
        m_frame_awaitable.reset();

        if (!result) {
            return std::unexpected(result.error());
        }

        if (!result.value()) {
            // 需要更多数据
            return false;
        }

        auto& frame = *result.value();
        auto stream = m_reader->m_conn->getStream(m_stream_id);

        switch (frame->type()) {
            case Http2FrameType::Settings: {
                auto* settings = static_cast<Http2SettingsFrame*>(frame.get());
                if (!settings->isAck()) {
                    m_reader->m_conn->peerSettings().applySettings(*settings);
                }
                return false;  // 继续读取
            }

            case Http2FrameType::Headers: {
                auto* hdrs = static_cast<Http2HeadersFrame*>(frame.get());
                if (frame->streamId() != m_stream_id) {
                    return false;  // 不是我们的流，继续读取
                }

                if (stream) {
                    stream->appendHeaderBlock(hdrs->headerBlock());
                    if (hdrs->isEndHeaders()) {
                        auto decode_result = m_reader->m_conn.decode(stream->headerBlock());
                        if (!decode_result) {
                            return std::unexpected(Http2ErrorCode::CompressionError);
                        }
                        stream->clearHeaderBlock();

                        for (const auto& field : *decode_result) {
                            if (field.name == ":status") {
                                m_response->status = std::stoi(field.value);
                            } else {
                                m_response->headers.push_back(field);
                            }
                        }

                        if (hdrs->isEndStream()) {
                            return true;  // 响应完成
                        }
                    }
                }
                return false;  // 继续读取
            }

            case Http2FrameType::Data: {
                auto* data = static_cast<Http2DataFrame*>(frame.get());
                if (frame->streamId() != m_stream_id) {
                    return false;  // 不是我们的流
                }

                m_response->body.append(data->data());

                if (data->isEndStream()) {
                    return true;  // 响应完成
                }
                return false;  // 继续读取
            }

            case Http2FrameType::WindowUpdate:
                return false;  // 继续读取

            case Http2FrameType::GoAway:
                return std::unexpected(Http2ErrorCode::ProtocolError);

            case Http2FrameType::RstStream:
                if (frame->streamId() == m_stream_id) {
                    return std::unexpected(Http2ErrorCode::StreamClosed);
                }
                return false;  // 继续读取

            default:
                return false;  // 忽略其他帧类型
        }
    }

private:
    Http2ReaderImpl<SocketType>* m_reader;
    uint32_t m_stream_id;
    Http2Response* m_response;
    std::optional<GetFrameAwaitableImpl<SocketType>> m_frame_awaitable;

public:
    std::optional<std::expected<void, IOError>> m_result;
};

/**
 * @brief HTTP/2 Reader
 */
template<typename SocketType>
class Http2ReaderImpl
{
    friend class GetFrameAwaitableImpl<SocketType>;
    friend class GetResponseAwaitableImpl<SocketType>;

public:
    Http2ReaderImpl(Http2ConnImpl<SocketType>& conn)
        : m_conn(&conn)
    {
    }

    /**
     * @brief 读取一个 HTTP/2 帧
     */
    GetFrameAwaitableImpl<SocketType> getFrame() {
        return GetFrameAwaitableImpl<SocketType>(*this);
    }

    /**
     * @brief 读取完整的 HTTP/2 响应（用于客户端）
     * @param stream_id 流 ID
     * @param response 输出参数，存储响应
     */
    GetResponseAwaitableImpl<SocketType> getResponse(uint32_t stream_id, Http2Response& response) {
        return GetResponseAwaitableImpl<SocketType>(*this, stream_id, response);
    }

private:
    Http2ConnImpl<SocketType>* m_conn;
};

// 类型别名
using Http2Reader = Http2ReaderImpl<galay::async::TcpSocket>;

#ifdef GALAY_HTTP_SSL_ENABLED
using Http2SslReader = Http2ReaderImpl<galay::ssl::SslSocket>;
#endif

} // namespace galay::http2

#endif // GALAY_HTTP2_READER_H
