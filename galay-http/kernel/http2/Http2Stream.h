#ifndef GALAY_HTTP2_STREAM_H
#define GALAY_HTTP2_STREAM_H

#include "galay-http/protoc/http2/Http2Base.h"
#include "galay-http/protoc/http2/Http2Frame.h"
#include "galay-http/protoc/http2/Http2Hpack.h"
#include "galay-kernel/concurrency/AsyncWaiter.h"
#include "galay-kernel/concurrency/UnsafeChannel.h"
#include "galay-kernel/kernel/Coroutine.h"
#include <string>
#include <vector>
#include <memory>

namespace galay::http2
{

template<typename SocketType>
class Http2StreamManagerImpl;

template<typename SocketType>
class Http2ConnImpl;

struct Http2OutgoingFrame {
    using Waiter = galay::kernel::AsyncWaiter<void>;
    using WaiterPtr = std::shared_ptr<Waiter>;

    Http2Frame::uptr frame;
    WaiterPtr waiter;

    Http2OutgoingFrame() = default;
    Http2OutgoingFrame(Http2Frame::uptr f, WaiterPtr w = nullptr)
        : frame(std::move(f))
        , waiter(std::move(w))
    {
    }
};

/**
 * @brief HTTP/2 请求结构
 */
struct Http2Request
{
    std::string method;
    std::string scheme;
    std::string authority;
    std::string path;
    std::vector<Http2HeaderField> headers;
    std::string body;
    
    // 获取伪头部
    std::string getHeader(const std::string& name) const {
        for (const auto& h : headers) {
            if (h.name == name) return h.value;
        }
        return "";
    }
};

/**
 * @brief HTTP/2 响应结构
 */
struct Http2Response
{
    int status = 200;
    std::vector<Http2HeaderField> headers;
    std::string body;
    
    void setHeader(const std::string& name, const std::string& value) {
        for (auto& h : headers) {
            if (h.name == name) {
                h.value = value;
                return;
            }
        }
        headers.push_back({name, value});
    }
    
    void setStatus(int code) { status = code; }
    void setBody(std::string data) { body = std::move(data); }
};

/**
 * @brief HTTP/2 流
 */
class Http2Stream
{
public:
    using ptr = std::shared_ptr<Http2Stream>;
    
    // 流 ID
    uint32_t streamId() const { return m_stream_id; }
    
    // 流状态
    Http2StreamState state() const { return m_state; }
    void setState(Http2StreamState state) { m_state = state; }
    
    // 流量控制窗口
    int32_t sendWindow() const { return m_send_window; }
    int32_t recvWindow() const { return m_recv_window; }
    
    void adjustSendWindow(int32_t delta) { m_send_window += delta; }
    void adjustRecvWindow(int32_t delta) { m_recv_window += delta; }
    
    // END_STREAM 标志
    bool isEndStreamReceived() const { return m_end_stream_received; }
    bool isEndStreamSent() const { return m_end_stream_sent; }
    void setEndStreamReceived() { m_end_stream_received = true; }
    void setEndStreamSent() { m_end_stream_sent = true; }
    
    // END_HEADERS 标志
    bool isEndHeadersReceived() const { return m_end_headers_received; }
    void setEndHeadersReceived() { m_end_headers_received = true; }
    
    // 头部块累积（用于 CONTINUATION）
    void appendHeaderBlock(const std::string& data) { m_header_block.append(data); }
    const std::string& headerBlock() const { return m_header_block; }
    void clearHeaderBlock() { m_header_block.clear(); }

    // 已解码的头部字段（由 StreamManager 在 readerLoop 中按帧顺序解码）
    void setDecodedHeaders(std::vector<Http2HeaderField> fields) { m_decoded_headers = std::move(fields); }
    const std::vector<Http2HeaderField>& decodedHeaders() const { return m_decoded_headers; }
    bool hasDecodedHeaders() const { return !m_decoded_headers.empty(); }
    void clearDecodedHeaders() { m_decoded_headers.clear(); }
    
    // 请求/响应数据
    Http2Request& request() { return m_request; }
    const Http2Request& request() const { return m_request; }
    
    Http2Response& response() { return m_response; }
    const Http2Response& response() const { return m_response; }
    
    // 数据累积
    void appendData(const std::string& data) { m_request.body.append(data); }
    
    // 状态转换
    bool canReceiveHeaders() const {
        return m_state == Http2StreamState::Idle || 
               m_state == Http2StreamState::ReservedRemote ||
               m_state == Http2StreamState::Open ||
               m_state == Http2StreamState::HalfClosedLocal;
    }
    
    bool canReceiveData() const {
        return m_state == Http2StreamState::Open ||
               m_state == Http2StreamState::HalfClosedLocal;
    }
    
    bool canSendHeaders() const {
        return m_state == Http2StreamState::Idle ||
               m_state == Http2StreamState::ReservedLocal ||
               m_state == Http2StreamState::Open ||
               m_state == Http2StreamState::HalfClosedRemote;
    }
    
    bool canSendData() const {
        return m_state == Http2StreamState::Open ||
               m_state == Http2StreamState::HalfClosedRemote;
    }
    
    // 处理接收到的帧后的状态转换
    void onHeadersReceived(bool end_stream) {
        if (m_state == Http2StreamState::Idle) {
            m_state = Http2StreamState::Open;
        }
        if (end_stream) {
            m_end_stream_received = true;
            if (m_state == Http2StreamState::Open) {
                m_state = Http2StreamState::HalfClosedRemote;
            } else if (m_state == Http2StreamState::HalfClosedLocal) {
                m_state = Http2StreamState::Closed;
            }
        }
    }
    
    void onDataReceived(bool end_stream) {
        if (end_stream) {
            m_end_stream_received = true;
            if (m_state == Http2StreamState::Open) {
                m_state = Http2StreamState::HalfClosedRemote;
            } else if (m_state == Http2StreamState::HalfClosedLocal) {
                m_state = Http2StreamState::Closed;
            }
        }
    }
    
    // 处理发送帧后的状态转换
    void onHeadersSent(bool end_stream) {
        if (m_state == Http2StreamState::Idle) {
            m_state = Http2StreamState::Open;
        } else if (m_state == Http2StreamState::ReservedLocal) {
            m_state = Http2StreamState::HalfClosedRemote;
        }
        if (end_stream) {
            m_end_stream_sent = true;
            if (m_state == Http2StreamState::Open) {
                m_state = Http2StreamState::HalfClosedLocal;
            } else if (m_state == Http2StreamState::HalfClosedRemote) {
                m_state = Http2StreamState::Closed;
            }
        }
    }
    
    void onDataSent(bool end_stream) {
        if (end_stream) {
            m_end_stream_sent = true;
            if (m_state == Http2StreamState::Open) {
                m_state = Http2StreamState::HalfClosedLocal;
            } else if (m_state == Http2StreamState::HalfClosedRemote) {
                m_state = Http2StreamState::Closed;
            }
        }
    }
    
    void onRstStreamReceived() {
        m_state = Http2StreamState::Closed;
    }
    
    void onRstStreamSent() {
        m_state = Http2StreamState::Closed;
    }

    // ==================== 帧队列 ====================

    /**
     * @brief 推入帧（由 StreamManager 调用）
     */
    void pushFrame(Http2Frame::uptr frame) {
        m_frame_channel.send(std::move(frame));
    }

    /**
     * @brief 标记帧队列关闭（由 StreamManager 在 RST_STREAM/GOAWAY 时调用）
     */
    void closeFrameQueue() {
        if (m_frame_queue_closed) return;
        m_frame_queue_closed = true;
        m_frame_channel.send(Http2Frame::uptr{});
    }

    bool isFrameQueueClosed() const { return m_frame_queue_closed; }

    /**
     * @brief 获取下一帧的 Awaitable
     * @return co_await 后得到 expected<uptr, IOError>，空指针表示流已关闭
     */
    auto getFrame() { return m_frame_channel.recv(); }

    // ==================== 发送接口 ====================

    /**
     * @brief 解码头部块
     */
    std::vector<Http2HeaderField> decodeHeaders(const std::string& header_block) {
        if (!m_decoder) return {};
        auto result = m_decoder->decode(header_block);
        if (!result) return {};
        return std::move(result.value());
    }

    /**
     * @brief 发送 HEADERS 帧
     */
    void sendHeaders(const std::vector<Http2HeaderField>& headers,
                     bool end_stream = false, bool end_headers = true) {
        sendHeadersInternal(headers, end_stream, end_headers, nullptr);
    }

    /**
     * @brief 发送 DATA 帧
     */
    void sendData(const std::string& data, bool end_stream = false) {
        sendDataInternal(data, end_stream, nullptr);
    }

    /**
     * @brief 发送 RST_STREAM 帧
     */
    void sendRstStream(Http2ErrorCode error) {
        if (!m_send_channel) return;

        auto frame = std::make_unique<Http2RstStreamFrame>();
        frame->header().stream_id = m_stream_id;
        frame->setErrorCode(error);

        onRstStreamSent();
        m_send_channel->send(Http2OutgoingFrame{std::move(frame)});
    }

    /**
     * @brief 发送 WINDOW_UPDATE 帧
     */
    void sendWindowUpdate(uint32_t increment) {
        if (!m_send_channel) return;

        auto frame = std::make_unique<Http2WindowUpdateFrame>();
        frame->header().stream_id = m_stream_id;
        frame->setWindowSizeIncrement(increment);

        m_send_channel->send(Http2OutgoingFrame{std::move(frame)});
    }

    /**
     * @brief 发送完整响应（HEADERS + DATA），默认结束流
     * @return 返回可等待对象，用于确认最后一帧写入完成
     */
    [[nodiscard]]
    Http2OutgoingFrame::WaiterPtr reply(const std::vector<Http2HeaderField>& headers,
                                        const std::string& body,
                                        bool end_headers = true) {
        auto waiter = std::make_shared<Http2OutgoingFrame::Waiter>();
        bool has_body = !body.empty();
        sendHeadersInternal(headers, !has_body, end_headers, has_body ? nullptr : waiter);
        if (has_body) {
            sendDataInternal(body, true, waiter);
        }
        return waiter;
    }

    /**
     * @brief 发送响应并等待发送完成
     */
    galay::kernel::Coroutine replyAndWait(const std::vector<Http2HeaderField>& headers,
                                          const std::string& body,
                                          bool end_headers = true) {
        auto waiter = reply(headers, body, end_headers);
        co_await waiter->wait();
        co_return;
    }

    /**
     * @brief 读取完整请求（HEADERS + DATA），填充 stream->request()
     * @return 协程完成时请求数据已填充到 request() 中
     */
    galay::kernel::Coroutine readRequest() {
        auto& req = m_request;
        while (true) {
            auto frame_result = co_await getFrame();
            if (!frame_result) co_return;
            auto frame = std::move(frame_result.value());
            if (!frame) co_return;

            if (frame->isHeaders()) {
                auto* hdrs = frame->asHeaders();
                if (hdrs->isEndHeaders()) {
                    // 使用 StreamManager 在 readerLoop 中已解码的字段
                    for (const auto& f : decodedHeaders()) {
                        if (f.name == ":method") req.method = f.value;
                        else if (f.name == ":scheme") req.scheme = f.value;
                        else if (f.name == ":authority") req.authority = f.value;
                        else if (f.name == ":path") req.path = f.value;
                        else req.headers.push_back({f.name, f.value});
                    }
                    clearDecodedHeaders();
                    if (hdrs->isEndStream()) co_return;
                }
            } else if (frame->isData()) {
                req.body.append(frame->asData()->data());
                if (frame->isEndStream()) co_return;
            } else if (frame->isContinuation()) {
                auto* cont = frame->asContinuation();
                if (cont->isEndHeaders()) {
                    for (const auto& f : decodedHeaders()) {
                        if (f.name == ":method") req.method = f.value;
                        else if (f.name == ":scheme") req.scheme = f.value;
                        else if (f.name == ":authority") req.authority = f.value;
                        else if (f.name == ":path") req.path = f.value;
                        else req.headers.push_back({f.name, f.value});
                    }
                    clearDecodedHeaders();
                }
            } else if (frame->isRstStream()) {
                co_return;
            }
        }
    }

    /**
     * @brief 读取完整响应（HEADERS + DATA），填充 stream->response()
     * @return 协程完成时响应数据已填充到 response() 中
     */
    galay::kernel::Coroutine readResponse() {
        auto& resp = m_response;
        while (true) {
            auto frame_result = co_await getFrame();
            if (!frame_result) co_return;
            auto frame = std::move(frame_result.value());
            if (!frame) co_return;

            if (frame->isHeaders()) {
                auto* hdrs = frame->asHeaders();
                if (hdrs->isEndHeaders()) {
                    for (const auto& f : decodedHeaders()) {
                        if (f.name == ":status") resp.status = std::stoi(f.value);
                        else resp.headers.push_back({f.name, f.value});
                    }
                    clearDecodedHeaders();
                    if (hdrs->isEndStream()) co_return;
                }
            } else if (frame->isData()) {
                resp.body.append(frame->asData()->data());
                if (frame->isEndStream()) co_return;
            } else if (frame->isContinuation()) {
                auto* cont = frame->asContinuation();
                if (cont->isEndHeaders()) {
                    for (const auto& f : decodedHeaders()) {
                        if (f.name == ":status") resp.status = std::stoi(f.value);
                        else resp.headers.push_back({f.name, f.value});
                    }
                    clearDecodedHeaders();
                }
            } else if (frame->isRstStream()) {
                co_return;
            }
        }
    }

    // ==================== 优先级 ====================

    uint8_t weight() const { return m_weight; }
    uint32_t streamDependency() const { return m_stream_dependency; }
    bool exclusive() const { return m_exclusive; }

    void setPriority(bool exclusive, uint32_t dependency, uint8_t weight) {
        m_exclusive = exclusive;
        m_stream_dependency = dependency;
        m_weight = weight;
    }

private:
    explicit Http2Stream(uint32_t stream_id)
        : m_stream_id(stream_id)
        , m_state(Http2StreamState::Idle)
        , m_send_window(kDefaultInitialWindowSize)
        , m_recv_window(kDefaultInitialWindowSize)
        , m_end_stream_received(false)
        , m_end_stream_sent(false)
        , m_end_headers_received(false)
    {
    }

    static ptr create(uint32_t stream_id) {
        return ptr(new Http2Stream(stream_id));
    }

    void attachIO(galay::kernel::UnsafeChannel<Http2OutgoingFrame>* send_channel,
                  HpackEncoder* encoder,
                  HpackDecoder* decoder) {
        m_send_channel = send_channel;
        m_encoder = encoder;
        m_decoder = decoder;
        m_io_attached = true;
    }

    uint32_t m_stream_id;
    Http2StreamState m_state;
    int32_t m_send_window;
    int32_t m_recv_window;
    bool m_end_stream_received;
    bool m_end_stream_sent;
    bool m_end_headers_received;
    std::string m_header_block;
    std::vector<Http2HeaderField> m_decoded_headers;
    Http2Request m_request;
    Http2Response m_response;

    // 帧通道
    galay::kernel::UnsafeChannel<Http2Frame::uptr> m_frame_channel;
    bool m_frame_queue_closed = false;

    // 优先级
    uint8_t m_weight = 16;
    uint32_t m_stream_dependency = 0;
    bool m_exclusive = false;

    // 发送通道和编解码器（由 StreamManager 绑定）
    galay::kernel::UnsafeChannel<Http2OutgoingFrame>* m_send_channel = nullptr;
    HpackEncoder* m_encoder = nullptr;
    HpackDecoder* m_decoder = nullptr;
    bool m_io_attached = false;

    template<typename SocketType>
    friend class Http2StreamManagerImpl;
    template<typename SocketType>
    friend class Http2ConnImpl;

    void sendHeadersInternal(const std::vector<Http2HeaderField>& headers,
                             bool end_stream,
                             bool end_headers,
                             const Http2OutgoingFrame::WaiterPtr& waiter) {
        if (!m_send_channel || !m_encoder) return;

        std::string header_block = m_encoder->encode(headers);

        auto frame = std::make_unique<Http2HeadersFrame>();
        frame->header().stream_id = m_stream_id;
        frame->setHeaderBlock(std::move(header_block));
        frame->setEndStream(end_stream);
        frame->setEndHeaders(end_headers);

        onHeadersSent(end_stream);
        m_send_channel->send(Http2OutgoingFrame{std::move(frame), waiter});
    }

    void sendDataInternal(const std::string& data,
                          bool end_stream,
                          const Http2OutgoingFrame::WaiterPtr& waiter) {
        if (!m_send_channel) return;

        auto frame = std::make_unique<Http2DataFrame>();
        frame->header().stream_id = m_stream_id;
        frame->setData(data);
        frame->setEndStream(end_stream);

        m_send_window -= static_cast<int32_t>(data.size());
        if (end_stream) {
            onDataSent(true);
        }
        m_send_channel->send(Http2OutgoingFrame{std::move(frame), waiter});
    }
};

} // namespace galay::http2

#endif // GALAY_HTTP2_STREAM_H
