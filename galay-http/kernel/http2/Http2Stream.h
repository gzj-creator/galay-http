#ifndef GALAY_HTTP2_STREAM_H
#define GALAY_HTTP2_STREAM_H

#include "galay-http/protoc/http2/Http2Base.h"
#include "galay-http/protoc/http2/Http2Frame.h"
#include "galay-http/protoc/http2/Http2Hpack.h"
#include "galay-http/protoc/http2/Http2Error.h"
#include "galay-kernel/concurrency/AsyncWaiter.h"
#include "galay-kernel/concurrency/UnsafeChannel.h"
#include <string>
#include <string_view>
#include <vector>
#include <charconv>
#include <memory>
#include <optional>
#include <iterator>

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
    std::string serialized;
    WaiterPtr waiter;

    Http2OutgoingFrame() = default;
    Http2OutgoingFrame(Http2Frame::uptr f, WaiterPtr w = nullptr)
        : frame(std::move(f))
        , waiter(std::move(w))
    {
    }

    Http2OutgoingFrame(std::string bytes, WaiterPtr w = nullptr)
        : serialized(std::move(bytes))
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

    class ReplyAndWaitAwaitable
        : public galay::kernel::TimeoutSupport<ReplyAndWaitAwaitable>
    {
    public:
        explicit ReplyAndWaitAwaitable(Http2OutgoingFrame::WaiterPtr waiter)
            : m_waiter(std::move(waiter))
            , m_wait_awaitable(m_waiter.get())
        {
        }

        bool await_ready() const noexcept {
            return m_wait_awaitable.await_ready();
        }

        template<typename Handle>
        bool await_suspend(Handle handle) {
            return m_wait_awaitable.await_suspend(handle);
        }

        ReplyAndWaitAwaitable& wait() & { return *this; }
        ReplyAndWaitAwaitable&& wait() && { return std::move(*this); }

        std::expected<void, galay::kernel::IOError> await_resume() {
            return m_wait_awaitable.await_resume();
        }

    private:
        Http2OutgoingFrame::WaiterPtr m_waiter;
        galay::kernel::AsyncWaiterAwaitable<void> m_wait_awaitable;
    };
    
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
        if (!m_frame_queue_enabled) {
            return;
        }
        m_frame_channel.send(std::move(frame));
    }

    /**
     * @brief 标记帧队列关闭（由 StreamManager 在 RST_STREAM/GOAWAY 时调用）
     */
    void closeFrameQueue() {
        if (m_frame_queue_closed) return;
        m_frame_queue_closed = true;
        markRequestCompleted();
        markResponseCompleted();
        m_frame_channel.send(Http2Frame::uptr{});
    }

    bool isFrameQueueClosed() const { return m_frame_queue_closed; }

    void setFrameQueueEnabled(bool enabled) { m_frame_queue_enabled = enabled; }
    bool isFrameQueueEnabled() const { return m_frame_queue_enabled; }

    void setGoAwayError(Http2GoAwayError error) { m_goaway_error = std::move(error); }
    bool hasGoAwayError() const { return m_goaway_error.has_value(); }
    const std::optional<Http2GoAwayError>& goAwayError() const { return m_goaway_error; }

    /**
     * @brief 获取下一帧的 Awaitable
     * @return co_await 后得到 expected<uptr, IOError>，空指针表示流已关闭
     */
    auto getFrame() {
        return m_frame_channel.recv();
    }

    /**
     * @brief 批量获取帧（至少 1 帧，最多 max_count）
     * @return co_await 后得到 expected<vector<uptr>, IOError>
     */
    auto getFrames(size_t max_count = galay::kernel::UnsafeChannel<Http2Frame::uptr>::DEFAULT_BATCH_SIZE) {
        return m_frame_channel.recvBatch(max_count);
    }

    /**
     * @brief 解码头部块
     */
    std::vector<Http2HeaderField> decodeHeaders(const std::string& header_block) {
        if (!m_decoder) return {};
        auto result = m_decoder->decode(header_block);
        if (!result) return {};
        return std::move(result.value());
    }

    void consumeDecodedHeadersAsRequest() {
        if (!m_decoded_headers.empty()) {
            m_request.headers.reserve(m_request.headers.size() + m_decoded_headers.size());
        }
        for (const auto& f : decodedHeaders()) {
            if (f.name == ":method") m_request.method = f.value;
            else if (f.name == ":scheme") m_request.scheme = f.value;
            else if (f.name == ":authority") m_request.authority = f.value;
            else if (f.name == ":path") m_request.path = f.value;
            else m_request.headers.push_back({f.name, f.value});
        }
        clearDecodedHeaders();
    }

    void consumeDecodedHeadersAsResponse() {
        if (!m_decoded_headers.empty()) {
            m_response.headers.reserve(m_response.headers.size() + m_decoded_headers.size());
        }
        for (const auto& f : decodedHeaders()) {
            if (f.name == ":status") {
                int status = 0;
                std::from_chars(f.value.data(), f.value.data() + f.value.size(), status);
                m_response.status = status;
            } else {
                m_response.headers.push_back({f.name, f.value});
            }
        }
        clearDecodedHeaders();
    }

    void appendRequestData(const std::string& data) {
        m_request.body.append(data);
    }

    void appendResponseData(const std::string& data) {
        m_response.body.append(data);
    }

    void markRequestCompleted() {
        if (m_request_completed) {
            return;
        }
        m_request_completed = true;
        m_request_waiter.notify();
    }

    void markResponseCompleted() {
        if (m_response_completed) {
            return;
        }
        m_response_completed = true;
        m_response_waiter.notify();
    }

    bool isRequestCompleted() const {
        return m_request_completed;
    }

    bool isResponseCompleted() const {
        return m_response_completed;
    }

    galay::kernel::AsyncWaiterAwaitable<void> waitRequestComplete() {
        return m_request_waiter.wait();
    }

    galay::kernel::AsyncWaiterAwaitable<void> waitResponseComplete() {
        return m_response_waiter.wait();
    }

    // ==================== 发送接口 ====================

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
        sendRstStreamInternal(error, nullptr);
    }

    /**
     * @brief 批量发送帧（按顺序入队）
     */
    void sendFrames(std::vector<Http2Frame::uptr> frames) {
        sendFrameBatchInternal(std::move(frames), nullptr);
    }

    /**
     * @brief 批量发送 DATA 帧（最后一帧可带 END_STREAM）
     */
    void sendDataBatch(const std::vector<std::string>& chunks, bool end_stream = false) {
        sendDataBatchInternal(chunks, end_stream, nullptr);
    }

    /**
     * @brief 帧优先 API：发送 HEADERS 并等待入队完成
     */
    ReplyAndWaitAwaitable replyHeader(const std::vector<Http2HeaderField>& headers,
                                      bool end_stream = false,
                                      bool end_headers = true) {
        auto waiter = std::make_shared<Http2OutgoingFrame::Waiter>();
        sendHeadersInternal(headers, end_stream, end_headers, waiter);
        return ReplyAndWaitAwaitable(std::move(waiter));
    }

    /**
     * @brief 帧优先 API：发送 DATA 并等待入队完成
     */
    ReplyAndWaitAwaitable replyData(const std::string& data, bool end_stream = false) {
        auto waiter = std::make_shared<Http2OutgoingFrame::Waiter>();
        sendDataInternal(data, end_stream, waiter);
        return ReplyAndWaitAwaitable(std::move(waiter));
    }

    /**
     * @brief 帧优先 API：发送 RST_STREAM 并等待入队完成
     */
    ReplyAndWaitAwaitable replyRst(Http2ErrorCode error) {
        auto waiter = std::make_shared<Http2OutgoingFrame::Waiter>();
        sendRstStreamInternal(error, waiter);
        return ReplyAndWaitAwaitable(std::move(waiter));
    }

    /**
     * @brief 帧优先 API：批量发送帧并等待“最后一帧入队”完成
     */
    ReplyAndWaitAwaitable replyFrames(std::vector<Http2Frame::uptr> frames) {
        auto waiter = std::make_shared<Http2OutgoingFrame::Waiter>();
        sendFrameBatchInternal(std::move(frames), waiter);
        return ReplyAndWaitAwaitable(std::move(waiter));
    }

    /**
     * @brief 帧优先 API：批量发送 DATA 并等待最后一帧入队
     */
    ReplyAndWaitAwaitable replyDataBatch(const std::vector<std::string>& chunks,
                                         bool end_stream = false) {
        auto waiter = std::make_shared<Http2OutgoingFrame::Waiter>();
        sendDataBatchInternal(chunks, end_stream, waiter);
        return ReplyAndWaitAwaitable(std::move(waiter));
    }

    /**
     * @brief 发送 WINDOW_UPDATE 帧
     */
    void sendWindowUpdate(uint32_t increment) {
        if (!m_send_queue && !m_send_channel) return;

        auto frame = std::make_unique<Http2WindowUpdateFrame>();
        frame->header().stream_id = m_stream_id;
        frame->setWindowSizeIncrement(increment);

        if (m_send_channel) {
            m_send_channel->send(Http2OutgoingFrame{std::move(frame)});
        } else {
            m_send_queue->push_back(Http2OutgoingFrame{std::move(frame)});
        }
    }

public:
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

    void attachIO(std::vector<Http2OutgoingFrame>* send_queue,
                  HpackEncoder* encoder,
                  HpackDecoder* decoder) {
        m_send_queue = send_queue;
        m_send_channel = nullptr;
        m_encoder = encoder;
        m_decoder = decoder;
        m_io_attached = true;
    }

    void attachIO(galay::kernel::UnsafeChannel<Http2OutgoingFrame>* send_channel,
                  HpackEncoder* encoder,
                  HpackDecoder* decoder) {
        m_send_channel = send_channel;
        m_send_queue = nullptr;
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
    bool m_frame_queue_enabled = true;
    std::optional<Http2GoAwayError> m_goaway_error;
    galay::kernel::AsyncWaiter<void> m_request_waiter;
    galay::kernel::AsyncWaiter<void> m_response_waiter;
    bool m_request_completed = false;
    bool m_response_completed = false;

    // 优先级
    uint8_t m_weight = 16;
    uint32_t m_stream_dependency = 0;
    bool m_exclusive = false;

    // 发送队列和编解码器（由 StreamManager 绑定）
    galay::kernel::UnsafeChannel<Http2OutgoingFrame>* m_send_channel = nullptr;
    std::vector<Http2OutgoingFrame>* m_send_queue = nullptr;
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
        if ((!m_send_queue && !m_send_channel) || !m_encoder) return;

        std::string header_block = m_encoder->encode(headers);
        auto bytes = Http2FrameBuilder::headersBytes(m_stream_id,
                                                     header_block,
                                                     end_stream,
                                                     end_headers);

        onHeadersSent(end_stream);
        if (m_send_channel) {
            m_send_channel->send(Http2OutgoingFrame{std::move(bytes), waiter});
        } else {
            m_send_queue->push_back(Http2OutgoingFrame{std::move(bytes), waiter});
        }
    }

    void sendDataInternal(const std::string& data,
                          bool end_stream,
                          const Http2OutgoingFrame::WaiterPtr& waiter) {
        if (!m_send_queue && !m_send_channel) return;
        if (m_send_window < static_cast<int32_t>(data.size())) return;

        auto bytes = Http2FrameBuilder::dataBytes(m_stream_id, data, end_stream);

        m_send_window -= static_cast<int32_t>(data.size());
        if (end_stream) {
            onDataSent(true);
        }
        if (m_send_channel) {
            m_send_channel->send(Http2OutgoingFrame{std::move(bytes), waiter});
        } else {
            m_send_queue->push_back(Http2OutgoingFrame{std::move(bytes), waiter});
        }
    }

    void sendRstStreamInternal(Http2ErrorCode error,
                               const Http2OutgoingFrame::WaiterPtr& waiter) {
        if (!m_send_queue && !m_send_channel) return;

        auto bytes = Http2FrameBuilder::rstStreamBytes(m_stream_id, error);

        onRstStreamSent();
        if (m_send_channel) {
            m_send_channel->send(Http2OutgoingFrame{std::move(bytes), waiter});
        } else {
            m_send_queue->push_back(Http2OutgoingFrame{std::move(bytes), waiter});
        }
    }

    void sendDataBatchInternal(const std::vector<std::string>& chunks,
                               bool end_stream,
                               const Http2OutgoingFrame::WaiterPtr& waiter) {
        if (!m_send_queue && !m_send_channel) {
            if (waiter) {
                waiter->notify();
            }
            return;
        }

        std::vector<Http2OutgoingFrame> outgoing;
        outgoing.reserve(chunks.size() + (chunks.empty() && end_stream ? 1 : 0));

        if (chunks.empty()) {
            if (end_stream) {
                onDataSent(true);
                outgoing.emplace_back(Http2FrameBuilder::dataBytes(m_stream_id, std::string_view{}, true));
            }
        } else {
            for (size_t i = 0; i < chunks.size(); ++i) {
                const auto& chunk = chunks[i];
                if (m_send_window < static_cast<int32_t>(chunk.size())) {
                    continue;
                }
                const bool last = end_stream && (i + 1 == chunks.size());
                m_send_window -= static_cast<int32_t>(chunk.size());
                if (last) {
                    onDataSent(true);
                }
                outgoing.emplace_back(Http2FrameBuilder::dataBytes(m_stream_id, chunk, last));
            }
        }

        if (outgoing.empty()) {
            if (waiter) {
                waiter->notify();
            }
            return;
        }

        if (waiter) {
            outgoing.back().waiter = waiter;
        }

        if (m_send_channel) {
            m_send_channel->sendBatch(std::move(outgoing));
        } else {
            m_send_queue->insert(m_send_queue->end(),
                                 std::make_move_iterator(outgoing.begin()),
                                 std::make_move_iterator(outgoing.end()));
        }
    }

    void sendFrameBatchInternal(std::vector<Http2Frame::uptr> frames,
                                const Http2OutgoingFrame::WaiterPtr& waiter) {
        if (!m_send_queue && !m_send_channel) {
            if (waiter) {
                waiter->notify();
            }
            return;
        }

        std::vector<Http2OutgoingFrame> outgoing;
        outgoing.reserve(frames.size());

        for (auto& frame : frames) {
            if (!frame) {
                continue;
            }

            frame->header().stream_id = m_stream_id;

            if (frame->isHeaders()) {
                onHeadersSent(frame->asHeaders()->isEndStream());
            } else if (frame->isData()) {
                auto* data = frame->asData();
                if (m_send_window < static_cast<int32_t>(data->data().size())) {
                    continue;
                }
                m_send_window -= static_cast<int32_t>(data->data().size());
                if (data->isEndStream()) {
                    onDataSent(true);
                }
            } else if (frame->isRstStream()) {
                onRstStreamSent();
            }

            outgoing.push_back(Http2OutgoingFrame{std::move(frame)});
        }

        if (outgoing.empty()) {
            if (waiter) {
                waiter->notify();
            }
            return;
        }

        if (waiter) {
            outgoing.back().waiter = waiter;
        }

        if (m_send_channel) {
            m_send_channel->sendBatch(std::move(outgoing));
            return;
        }

        m_send_queue->insert(
            m_send_queue->end(),
            std::make_move_iterator(outgoing.begin()),
            std::make_move_iterator(outgoing.end()));
    }
};

} // namespace galay::http2

#endif // GALAY_HTTP2_STREAM_H
