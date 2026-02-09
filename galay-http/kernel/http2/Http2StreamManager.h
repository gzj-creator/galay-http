#ifndef GALAY_HTTP2_STREAM_MANAGER_H
#define GALAY_HTTP2_STREAM_MANAGER_H

#include "Http2Conn.h"
#include "Http2Stream.h"
#include "galay-http/protoc/http2/Http2Base.h"
#include "galay-http/protoc/http2/Http2Frame.h"
#include "galay-http/kernel/http/HttpLog.h"
#include "galay-kernel/concurrency/AsyncWaiter.h"
#include "galay-kernel/kernel/Coroutine.h"
#include "galay-kernel/concurrency/UnsafeChannel.h"
#include <memory>
#include <queue>
#include <string>
#include <vector>
#include <optional>
#include <coroutine>
#include <functional>
#include <type_traits>
#include <atomic>

namespace galay::http2
{

using namespace galay::kernel;

/**
 * @brief 按优先级排序的流比较器
 * weight 越大优先级越高（大顶堆）
 */
struct StreamPriorityCompare {
    bool operator()(const Http2Stream::ptr& a, const Http2Stream::ptr& b) const {
        return a->weight() < b->weight();
    }
};

/**
 * @brief 待处理的连接级动作（由非协程函数标记，由主循环执行）
 */
struct PendingAction {
    enum class Type {
        SendGoaway,
        SendRstStream,
        SendWindowUpdate
    };
    Type type;
    uint32_t stream_id = 0;
    Http2ErrorCode error_code = Http2ErrorCode::NoError;
    uint32_t increment = 0;
};

/**
 * @brief 用户流处理器类型
 */
using Http2StreamHandler = std::function<Coroutine(Http2Stream::ptr)>;

/**
 * @brief HTTP/2 流管理器
 *
 * 职责：
 * 1. 运行帧读取协程（Reader），处理连接级帧，分发流级帧到对应 Http2Stream
 * 2. 运行帧写入协程（Writer），从 UnsafeChannel 接收帧并发送到 socket
 * 3. 新流创建后自动 spawn 用户 handler
 */
template<typename SocketType>
class Http2StreamManagerImpl
{
public:
    Http2StreamManagerImpl(Http2ConnImpl<SocketType>& conn)
        : m_conn(conn)
        , m_running(false)
    {
    }

    /**
     * @brief 启动流管理器（协程）
     * @param handler 用户流处理回调，每个新流创建后 spawn handler(stream)
     *
     * 内部启动两个协程：
     * - Reader: 读取帧、处理连接级帧、分发流级帧、spawn handler
     * - Writer: 从 send channel 接收数据并写入 socket
     */
    Coroutine start(Http2StreamHandler handler) {
        m_started = true;
        m_running = true;
        m_draining_handlers.store(false, std::memory_order_release);

        // 初始化自动分配的 stream ID
        if (m_next_local_stream_id == 0) {
            m_next_local_stream_id = m_conn.isClient() ? 3 : 2;
        }

        // spawn writer 协程（后台运行），保存句柄用于等待结束
        Coroutine writer = writerLoop();
        co_await spawn(writer);
        m_writer_ready.notify();

        // reader 协程在当前协程中运行
        co_await readerLoop(std::move(handler)).wait();

        // reader 退出后，等待所有 handler 完成，避免 stream 使用已销毁的 manager
        m_draining_handlers.store(true, std::memory_order_release);
        if (m_active_handlers.load(std::memory_order_acquire) > 0) {
            co_await m_handler_waiter.wait();
        }

        // reader 退出后，发送关闭信号给 writer，并等待其退出
        m_send_channel.send(Http2OutgoingFrame{});
        co_await writer.wait();

        m_running = false;
        m_stop_waiter.notify();
        co_return;
    }

    /**
     * @brief 将帧入队发送
     */
    void enqueueSendFrame(Http2Frame::uptr frame,
                          const Http2OutgoingFrame::WaiterPtr& waiter = nullptr) {
        m_send_channel.send(Http2OutgoingFrame{std::move(frame), waiter});
    }

    template<typename FrameType>
    void enqueueSendFrame(FrameType&& frame,
                          const Http2OutgoingFrame::WaiterPtr& waiter = nullptr) {
        using FrameT = std::decay_t<FrameType>;
        static_assert(std::is_base_of_v<Http2Frame, FrameT>, "FrameType must derive from Http2Frame");
        m_send_channel.send(
            Http2OutgoingFrame{std::make_unique<FrameT>(std::forward<FrameType>(frame)), waiter});
    }

    bool isRunning() const { return m_running; }

    /**
     * @brief 获取连接引用（供用户 handler 使用）
     */
    Http2ConnImpl<SocketType>& conn() { return m_conn; }

    /**
     * @brief 在后台启动 StreamManager，等待 writer 就绪后返回
     * @details 比 co_await spawn(mgr->start(handler)) 更安全：
     *          返回时 writer 已启动，可以安全地向 send channel 推送帧。
     *          调用者之后可以直接 sendHeaders/sendData，无需担心调度时序。
     */
    Coroutine startInBackground(Http2StreamHandler handler) {
        co_await spawn(start(std::move(handler)));
        co_await m_writer_ready.wait();
        co_return;
    }

    /**
     * @brief 自动分配 stream ID 并创建流
     * @details 客户端自动分配奇数 ID（3, 5, 7, ...），服务端自动分配偶数 ID（2, 4, 6, ...）
     */
    Http2Stream::ptr allocateStream() {
        uint32_t id = m_next_local_stream_id;
        m_next_local_stream_id += 2;
        return newStream(id);
    }

    /**
     * @brief 优雅关闭：发送 GOAWAY、关闭连接、等待 StreamManager 停止
     * @details 替代手动的 sendGoaway + conn.close() + waitStopped() 序列
     */
    Coroutine shutdown(Http2ErrorCode error = Http2ErrorCode::NoError) {
        if (!m_started) co_return;

        if (m_running) {
            auto waiter = sendGoaway(error);
            if (waiter) {
                co_await waiter->wait();
            }

            // 关闭所有流的帧队列
            m_conn.forEachStream([](uint32_t, Http2Stream::ptr& stream) {
                stream->closeFrameQueue();
            });

            // close() 设置 m_closing=true 并关闭 socket。
            // readerLoop 的 readFrame() 在 await_ready() 中检测到 closing，
            // 不再挂起，直接返回错误，readerLoop 退出。
            co_await m_conn.close();
            if (m_running) {
                co_await waitStopped();
            }
        }
        co_return;
    }

    /**
     * @brief 发送 GOAWAY 帧
     * @return waiter，co_await waiter->wait() 等待发送完成
     */
    Http2OutgoingFrame::WaiterPtr sendGoaway(Http2ErrorCode error = Http2ErrorCode::NoError,
                                             const std::string& debug = "") {
        auto waiter = std::make_shared<Http2OutgoingFrame::Waiter>();
        enqueueGoaway(error, debug, waiter);
        return waiter;
    }

    /**
     * @brief 等待 StreamManager 停止（start() 完成）
     */
    galay::kernel::AsyncWaiterAwaitable<void> waitStopped() {
        return m_stop_waiter.wait();
    }

private:
    Http2Stream::ptr newStream(uint32_t stream_id) {
        auto stream = m_conn.getStream(stream_id);
        if (stream) {
            attachStreamIO(stream);
            return stream;
        }
        return createStreamInternal(stream_id);
    }

    /**
     * @brief Reader 协程：读取帧、处理连接级帧、分发流级帧
     */
    Coroutine readerLoop(Http2StreamHandler handler) {
        // readerLoop 只在 IO 错误（peer closed / connection error）或连接关闭时退出。
        // GOAWAY（无论收到还是发出）不退出：GOAWAY 只表示不再有新流，
        // 已有流的帧仍需继续读取直到连接关闭（RFC 9113 §6.8）。
        while (true) {
            auto read_awaitable = m_conn.readFrame();
            auto frame_result = co_await read_awaitable;

            if (!frame_result) {
                if (m_conn.isClosing() || m_conn.isPeerClosed()) {
                    HTTP_LOG_INFO("[stream-mgr] [reader] [exit] [{}] [{}]",
                                  m_conn.isPeerClosed() ? "peer-closed" : "closing",
                                  m_conn.lastReadError());
                    break;
                }
                if (frame_result.error() == Http2ErrorCode::NoError) {
                    continue;
                }
                if (frame_result.error() == Http2ErrorCode::ProtocolError &&
                    (m_conn.isPeerClosed() || m_conn.isClosing())) {
                    HTTP_LOG_INFO("[stream-mgr] [reader] [exit] [{}] [{}]",
                                  m_conn.isPeerClosed() ? "peer-closed" : "closing",
                                  m_conn.lastReadError());
                    break;
                }
                if (m_conn.lastReadError().empty()) {
                    HTTP_LOG_ERROR("[stream-mgr] [frame] [read-fail] [{}]",
                                  http2ErrorCodeToString(frame_result.error()));
                } else {
                    HTTP_LOG_ERROR("[stream-mgr] [frame] [read-fail] [{}] [{}]",
                                  http2ErrorCodeToString(frame_result.error()),
                                  m_conn.lastReadError());
                }
                enqueueGoaway(frame_result.error());
                break;
            }

            auto& frame = *frame_result;
            uint32_t stream_id = frame->streamId();

            HTTP_LOG_DEBUG("[stream-mgr] [frame] [recv] [type={}] [stream={}] [flags=0x{:02x}]",
                          http2FrameTypeToString(frame->type()), stream_id, frame->header().flags);

            // CONTINUATION 状态检查
            if (m_conn.isExpectingContinuation()) {
                if (!frame->isContinuation() || stream_id != m_conn.continuationStreamId()) {
                    enqueueGoaway(Http2ErrorCode::ProtocolError);
                    break;
                }
            }

            // 连接级帧
            if (frame->isSettings() || frame->isPing() || frame->isGoAway() ||
                (frame->isWindowUpdate() && stream_id == 0)) {
                handleConnectionFrame(std::move(frame));
                continue;
            }

            // 流级帧 → 分发到 Http2Stream 帧队列
            dispatchStreamFrame(std::move(frame));

            // 处理 dispatchStreamFrame 中标记的待处理动作
            processPendingActions();

            // spawn 待处理的流 handler
            while (!m_pending_spawns.empty()) {
                auto stream = m_pending_spawns.top();
                m_pending_spawns.pop();
                m_active_handlers.fetch_add(1, std::memory_order_acq_rel);
                co_await spawn(runHandler(handler, stream));
            }
        }

        // 关闭所有流的帧队列
        m_conn.forEachStream([](uint32_t, Http2Stream::ptr& stream) {
            stream->closeFrameQueue();
        });

        co_return;
    }

    Coroutine runHandler(Http2StreamHandler handler, Http2Stream::ptr stream) {
        co_await handler(stream).wait();
        // 流处理完毕，从连接的流表中移除，释放 max_concurrent_streams 配额
        m_conn.removeStream(stream->streamId());
        int remaining = m_active_handlers.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0 && m_draining_handlers.load(std::memory_order_acquire)) {
            m_handler_waiter.notify();
        }
        co_return;
    }

    /**
     * @brief Writer 协程：从 send channel 接收数据并写入 socket
     * @details 使用 writev 批量发送多个帧，减少系统调用和内存拷贝
     */
    Coroutine writerLoop() {
        // 预分配序列化缓冲区和 iovec 数组，避免每次循环分配
        std::vector<std::string> frame_buffers;
        std::vector<iovec> iovecs;
        frame_buffers.reserve(64);
        iovecs.reserve(64);

        while (true) {
            auto batch_result = co_await m_send_channel.recvBatch();
            if (!batch_result) {
                HTTP_LOG_ERROR("[stream-mgr] [writer] [recv-fail]");
                break;
            }

            auto& batch = *batch_result;
            frame_buffers.clear();
            iovecs.clear();

            // 批量序列化所有帧到 iovec
            bool has_shutdown = false;
            for (auto& item : batch) {
                if (!item.frame) {
                    // 收到关闭信号，先发送已有数据再退出
                    HTTP_LOG_DEBUG("[stream-mgr] [writer] [shutdown]");
                    has_shutdown = true;
                    break;
                }

                frame_buffers.push_back(item.frame->serialize());
                iovecs.push_back({
                    .iov_base = frame_buffers.back().data(),
                    .iov_len = frame_buffers.back().size()
                });
            }

            // 使用 writev 一次性发送所有帧
            if (!iovecs.empty()) {
                size_t total_bytes = 0;
                for (const auto& iov : iovecs) {
                    total_bytes += iov.iov_len;
                }

                size_t sent = 0;
                while (sent < total_bytes) {
                    auto result = co_await m_conn.socket().writev(iovecs);
                    if (!result) {
                        if (m_conn.isClosing() || m_conn.isPeerClosed() ||
                            m_conn.isGoawaySent() || m_conn.isGoawayReceived()) {
                            HTTP_LOG_DEBUG("[stream-mgr] [writer] [writev-fail] [closing]");
                        } else {
                            HTTP_LOG_ERROR("[stream-mgr] [writer] [writev-fail]");
                        }
                        // 通知所有 waiter 发送失败
                        for (auto& item : batch) {
                            if (item.waiter) {
                                item.waiter->notify();
                            }
                        }
                        co_return;
                    }

                    sent += result.value();
                    if (sent >= total_bytes) {
                        break;
                    }

                    // 部分发送，调整 iovecs
                    size_t remaining = result.value();
                    for (auto& iov : iovecs) {
                        if (remaining >= iov.iov_len) {
                            remaining -= iov.iov_len;
                            iov.iov_len = 0;
                        } else {
                            iov.iov_base = static_cast<char*>(iov.iov_base) + remaining;
                            iov.iov_len -= remaining;
                            break;
                        }
                    }
                    // 移除已发送完的 iovec
                    iovecs.erase(
                        std::remove_if(iovecs.begin(), iovecs.end(),
                                      [](const iovec& iov) { return iov.iov_len == 0; }),
                        iovecs.end());
                }
            }

            // 通知所有 waiter
            for (auto& item : batch) {
                if (item.waiter) {
                    item.waiter->notify();
                }
            }

            if (has_shutdown) {
                co_return;
            }
        }

        co_return;
    }

    /**
     * @brief 处理连接级帧（非协程，通过 channel 发送响应）
     */
    void handleConnectionFrame(Http2Frame::uptr frame) {
        switch (frame->type()) {
            case Http2FrameType::Settings: {
                auto* settings = frame->asSettings();
                if (settings->isAck()) {
                    HTTP_LOG_DEBUG("[stream-mgr] [settings] [ack]");
                } else {
                    m_conn.peerSettings().applySettings(*settings);
                    m_conn.encoder().setMaxTableSize(m_conn.peerSettings().header_table_size);

                    Http2SettingsFrame ack;
                    ack.setAck(true);
                    enqueueSendFrame(std::move(ack));
                    HTTP_LOG_DEBUG("[stream-mgr] [settings] [ack-enqueued]");
                }
                break;
            }

            case Http2FrameType::Ping: {
                auto* ping = frame->asPing();
                if (frame->streamId() != 0) {
                    enqueueGoaway(Http2ErrorCode::ProtocolError);
                    return;
                }
                if (!ping->isAck()) {
                    Http2PingFrame pong;
                    pong.setOpaqueData(ping->opaqueData());
                    pong.setAck(true);
                    enqueueSendFrame(std::move(pong));
                }
                break;
            }

            case Http2FrameType::GoAway: {
                auto* goaway = frame->asGoAway();
                m_conn.setGoawayReceived();
                HTTP_LOG_INFO("[stream-mgr] [goaway] [recv] [last={}] [err={}] [debug={}]",
                             goaway->lastStreamId(),
                             http2ErrorCodeToString(goaway->errorCode()),
                             goaway->debugData());
                break;
            }

            case Http2FrameType::WindowUpdate: {
                auto* wu = frame->asWindowUpdate();
                uint32_t increment = wu->windowSizeIncrement();
                if (increment == 0) {
                    enqueueGoaway(Http2ErrorCode::ProtocolError);
                    return;
                }
                m_conn.adjustConnSendWindow(increment);
                break;
            }

            default:
                break;
        }
    }

    /**
     * @brief 分发流级帧到对应 Http2Stream 的帧队列
     */
    void dispatchStreamFrame(Http2Frame::uptr frame) {
        uint32_t stream_id = frame->streamId();

        if (stream_id == 0) {
            return;
        }

        // HEADERS 帧
        if (frame->isHeaders()) {
            auto stream = m_conn.getStream(stream_id);
            attachStreamIO(stream);
            if (!stream) {
                if (m_conn.isClient()) {
                    HTTP_LOG_WARN("[stream-mgr] [client] [headers] [unknown-stream={}]", stream_id);
                    m_pending_actions.push_back({PendingAction::Type::SendGoaway, 0, Http2ErrorCode::ProtocolError});
                    return;
                }
                // 服务端模式：对端发起的新请求
                if (stream_id <= m_conn.lastPeerStreamId()) {
                    m_pending_actions.push_back({PendingAction::Type::SendGoaway, 0, Http2ErrorCode::ProtocolError});
                    return;
                }
                if (m_conn.streamCount() >= m_conn.localSettings().max_concurrent_streams) {
                    m_pending_actions.push_back({PendingAction::Type::SendRstStream, stream_id, Http2ErrorCode::RefusedStream});
                    return;
                }
                stream = createStreamInternal(stream_id);
                m_conn.setLastPeerStreamId(stream_id);
            }

            if (!stream->canReceiveHeaders()) {
                m_pending_actions.push_back({PendingAction::Type::SendRstStream, stream_id, Http2ErrorCode::StreamClosed});
                return;
            }

            auto* hdrs = frame->asHeaders();
            if (hdrs->hasPriority()) {
                stream->setPriority(hdrs->exclusive(), hdrs->streamDependency(), hdrs->weight());
            }

            bool end_headers = hdrs->isEndHeaders();
            stream->onHeadersReceived(hdrs->isEndStream());
            stream->appendHeaderBlock(hdrs->headerBlock());
            stream->pushFrame(std::move(frame));

            if (end_headers) {
                // HPACK 解码必须在 readerLoop 中按帧到达顺序执行，
                // 不能延迟到各 handler 协程中并发解码（动态表会错乱）
                auto fields = m_conn.decoder().decode(stream->headerBlock());
                if (fields) {
                    stream->setDecodedHeaders(std::move(fields.value()));
                }
                stream->clearHeaderBlock();
                m_conn.setExpectingContinuation(false);
                if (!m_conn.isClient()) {
                    // 服务端：新请求就绪，spawn handler
                    queueStreamHandler(stream);
                }
            } else {
                m_conn.setExpectingContinuation(true, stream_id);
            }
            return;
        }

        // CONTINUATION 帧
        if (frame->isContinuation()) {
            auto stream = m_conn.getStream(stream_id);
            attachStreamIO(stream);
            if (!stream) {
                m_pending_actions.push_back({PendingAction::Type::SendGoaway, 0, Http2ErrorCode::ProtocolError});
                return;
            }

            auto* cont = frame->asContinuation();
            bool end_headers = cont->isEndHeaders();
            stream->appendHeaderBlock(cont->headerBlock());
            stream->pushFrame(std::move(frame));

            if (end_headers) {
                auto fields = m_conn.decoder().decode(stream->headerBlock());
                if (fields) {
                    stream->setDecodedHeaders(std::move(fields.value()));
                }
                stream->clearHeaderBlock();
                m_conn.setExpectingContinuation(false);
                if (!m_conn.isClient()) {
                    queueStreamHandler(stream);
                }
            }
            return;
        }

        // DATA 帧
        if (frame->isData()) {
            if (stream_id == 0) {
                m_pending_actions.push_back({PendingAction::Type::SendGoaway, 0, Http2ErrorCode::ProtocolError});
                return;
            }

            auto stream = m_conn.getStream(stream_id);
            attachStreamIO(stream);
            if (!stream) {
                m_pending_actions.push_back({PendingAction::Type::SendRstStream, stream_id, Http2ErrorCode::StreamClosed});
                return;
            }

            if (!stream->canReceiveData()) {
                m_pending_actions.push_back({PendingAction::Type::SendRstStream, stream_id, Http2ErrorCode::StreamClosed});
                return;
            }

            auto* data = frame->asData();
            stream->onDataReceived(data->isEndStream());
            int32_t data_size = static_cast<int32_t>(data->data().size());

            // 流量控制
            m_conn.adjustConnRecvWindow(-data_size);
            stream->adjustRecvWindow(-data_size);

            // 标记需要发送 WINDOW_UPDATE
            if (m_conn.connRecvWindow() < static_cast<int32_t>(kDefaultInitialWindowSize / 2)) {
                uint32_t inc = kDefaultInitialWindowSize - m_conn.connRecvWindow();
                m_pending_actions.push_back({PendingAction::Type::SendWindowUpdate, 0, Http2ErrorCode::NoError, inc});
                m_conn.adjustConnRecvWindow(inc);
            }
            if (stream->recvWindow() < static_cast<int32_t>(kDefaultInitialWindowSize / 2)) {
                uint32_t inc = kDefaultInitialWindowSize - stream->recvWindow();
                m_pending_actions.push_back({PendingAction::Type::SendWindowUpdate, stream_id, Http2ErrorCode::NoError, inc});
                stream->adjustRecvWindow(inc);
            }

            stream->pushFrame(std::move(frame));
            return;
        }

        // PRIORITY 帧
        if (frame->isPriority()) {
            auto stream = m_conn.getStream(stream_id);
            attachStreamIO(stream);
            if (stream) {
                auto* prio = frame->asPriority();
                stream->setPriority(prio->exclusive(), prio->streamDependency(), prio->weight());
            }
            return;
        }

        // RST_STREAM 帧
        if (frame->isRstStream()) {
            if (stream_id == 0) {
                m_pending_actions.push_back({PendingAction::Type::SendGoaway, 0, Http2ErrorCode::ProtocolError});
                return;
            }
            auto stream = m_conn.getStream(stream_id);
            attachStreamIO(stream);
            if (stream) {
                stream->onRstStreamReceived();
                HTTP_LOG_DEBUG("[stream-mgr] [stream] [rst] [id={}] [err={}]",
                              stream_id, http2ErrorCodeToString(frame->asRstStream()->errorCode()));
                stream->pushFrame(std::move(frame));
                stream->closeFrameQueue();
            }
            return;
        }

        // WINDOW_UPDATE on stream > 0
        if (frame->isWindowUpdate()) {
            auto stream = m_conn.getStream(stream_id);
            attachStreamIO(stream);
            if (stream) {
                auto* wu = frame->asWindowUpdate();
                uint32_t increment = wu->windowSizeIncrement();
                if (increment == 0) {
                    m_pending_actions.push_back({PendingAction::Type::SendRstStream, stream_id, Http2ErrorCode::ProtocolError});
                    return;
                }
                stream->adjustSendWindow(increment);
                stream->pushFrame(std::move(frame));
            }
            return;
        }

        // PUSH_PROMISE
        if (frame->isPushPromise()) {
            if (!m_conn.isClient()) {
                m_pending_actions.push_back({PendingAction::Type::SendGoaway, 0, Http2ErrorCode::ProtocolError});
                return;
            }
            auto* pp = frame->asPushPromise();
            uint32_t promised_id = pp->promisedStreamId();
            auto promised_stream = m_conn.getStream(promised_id);
            attachStreamIO(promised_stream);
            if (!promised_stream) {
                promised_stream = createStreamInternal(promised_id);
                promised_stream->setState(Http2StreamState::ReservedRemote);
            }
            promised_stream->pushFrame(std::move(frame));
            queueStreamHandler(promised_stream);
            return;
        }

        HTTP_LOG_WARN("[stream-mgr] [frame] [unknown] [type={}]", static_cast<int>(frame->type()));
    }

    /**
     * @brief 处理 dispatchStreamFrame 中标记的待处理动作（通过 channel 发送）
     */
    void processPendingActions() {
        while (!m_pending_actions.empty()) {
            auto action = m_pending_actions.front();
            m_pending_actions.erase(m_pending_actions.begin());

            switch (action.type) {
                case PendingAction::Type::SendGoaway: {
                    enqueueGoaway(action.error_code);
                    break;
                }
                case PendingAction::Type::SendRstStream: {
                    Http2RstStreamFrame frame;
                    frame.header().stream_id = action.stream_id;
                    frame.setErrorCode(action.error_code);
                    auto stream = m_conn.getStream(action.stream_id);
                    if (stream) {
                        stream->onRstStreamSent();
                    }
                    enqueueSendFrame(std::move(frame));
                    break;
                }
                case PendingAction::Type::SendWindowUpdate: {
                    Http2WindowUpdateFrame frame;
                    frame.header().stream_id = action.stream_id;
                    frame.setWindowSizeIncrement(action.increment);
                    enqueueSendFrame(std::move(frame));
                    break;
                }
            }
        }
    }

    /**
     * @brief 入队 GOAWAY 帧
     */
    void enqueueGoaway(Http2ErrorCode error,
                       const std::string& debug = "",
                       const Http2OutgoingFrame::WaiterPtr& waiter = nullptr) {
        Http2GoAwayFrame frame;
        frame.setLastStreamId(m_conn.lastPeerStreamId());
        frame.setErrorCode(error);
        if (!debug.empty()) {
            frame.setDebugData(debug);
        }
        m_conn.setGoawaySent();
        enqueueSendFrame(std::move(frame), waiter);
    }

    /**
     * @brief 将新流加入待 spawn 队列
     */
    void queueStreamHandler(Http2Stream::ptr stream) {
        m_pending_spawns.push(stream);
    }

    Http2Stream::ptr createStreamInternal(uint32_t stream_id) {
        auto stream = m_conn.createStream(stream_id);
        attachStreamIO(stream);
        return stream;
    }

    void attachStreamIO(const Http2Stream::ptr& stream) {
        if (!stream) return;
        stream->attachIO(&m_send_channel, &m_conn.encoder(), &m_conn.decoder());
    }

    Http2ConnImpl<SocketType>& m_conn;
    bool m_started = false;
    bool m_running;
    galay::kernel::AsyncWaiter<void> m_stop_waiter;
    galay::kernel::AsyncWaiter<void> m_writer_ready;
    uint32_t m_next_local_stream_id = 0;
    std::atomic<int> m_active_handlers{0};
    std::atomic<bool> m_draining_handlers{false};
    galay::kernel::AsyncWaiter<void> m_handler_waiter;

    // 发送通道：空指针表示关闭信号
    UnsafeChannel<Http2OutgoingFrame> m_send_channel;

    // 待处理动作队列
    std::vector<PendingAction> m_pending_actions;

    // 待 spawn 的流队列（按优先级排序）
    std::priority_queue<Http2Stream::ptr, std::vector<Http2Stream::ptr>, StreamPriorityCompare> m_pending_spawns;
};

// 类型别名
using Http2StreamManager = Http2StreamManagerImpl<galay::async::TcpSocket>;

} // namespace galay::http2

#endif // GALAY_HTTP2_STREAM_MANAGER_H
