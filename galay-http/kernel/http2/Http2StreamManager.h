#ifndef GALAY_HTTP2_STREAM_MANAGER_H
#define GALAY_HTTP2_STREAM_MANAGER_H

#include "Http2Conn.h"
#include "Http2Stream.h"
#include "galay-http/protoc/http2/Http2Base.h"
#include "galay-http/protoc/http2/Http2Frame.h"
#include "galay-http/kernel/IoVecUtils.h"
#include "galay-http/kernel/http/HttpLog.h"
#include "galay-kernel/concurrency/AsyncWaiter.h"
#include "galay-kernel/kernel/Coroutine.h"
#include "galay-kernel/common/Sleep.hpp"
#include <memory>
#include <queue>
#include <string>
#include <vector>
#include <optional>
#include <coroutine>
#include <functional>
#include <type_traits>
#include <atomic>
#include <chrono>
#include <array>
#include <cstring>
#include <algorithm>
#include <deque>

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

class Http2ActiveStreamBatch {
public:
    void mark(const Http2Stream::ptr& stream, Http2StreamEvent events) {
        if (!stream || events == Http2StreamEvent::None) {
            return;
        }
        stream->m_pending_events |= events;
        if (stream->m_active_queued) {
            return;
        }
        stream->m_active_queued = true;
        m_ready.push_back(stream);
    }

    std::vector<Http2Stream::ptr> takeReady() {
        std::vector<Http2Stream::ptr> ready;
        ready.swap(m_ready);
        return ready;
    }

    bool empty() const {
        return m_ready.empty();
    }

private:
    std::vector<Http2Stream::ptr> m_ready;
};

class Http2ActiveStreamMailbox {
public:
    using Batch = std::vector<Http2Stream::ptr>;
    using BatchResult = std::optional<Batch>;

    class RecvBatchAwaitable {
    public:
        RecvBatchAwaitable(Http2ActiveStreamMailbox* mailbox, size_t max_count)
            : m_mailbox(mailbox)
            , m_max_count(std::max<size_t>(max_count, 1))
        {
        }

        bool await_ready() const noexcept {
            return m_mailbox->m_closed || !m_mailbox->m_batches.empty();
        }

        template<typename Handle>
        bool await_suspend(Handle handle) noexcept {
            if (m_mailbox->m_closed || !m_mailbox->m_batches.empty()) {
                return false;
            }
            m_mailbox->m_waiterHandle = handle;
            m_mailbox->m_hasWaiter = true;
            return true;
        }

        BatchResult await_resume() {
            return m_mailbox->popBatch(m_max_count);
        }

    private:
        Http2ActiveStreamMailbox* m_mailbox;
        size_t m_max_count;
    };

    void sendBatch(Batch&& batch) {
        if (m_closed || batch.empty()) {
            return;
        }
        m_batches.push_back(std::move(batch));
        wakeWaiter();
    }

    RecvBatchAwaitable recvBatch(
        size_t max_count = galay::kernel::UnsafeChannel<Http2Stream::ptr>::DEFAULT_BATCH_SIZE) {
        return RecvBatchAwaitable(this, max_count);
    }

    void close() {
        if (m_closed) {
            return;
        }
        m_closed = true;
        wakeWaiter();
    }

    void reset() {
        m_batches.clear();
        m_closed = false;
        m_hasWaiter = false;
        m_waiterHandle = {};
    }

private:
    friend class RecvBatchAwaitable;

    BatchResult popBatch(size_t max_count) {
        if (m_batches.empty()) {
            if (m_closed) {
                return std::nullopt;
            }
            return Batch{};
        }

        auto& front = m_batches.front();
        if (front.size() <= max_count) {
            Batch batch = std::move(front);
            m_batches.pop_front();
            return batch;
        }

        Batch batch;
        batch.reserve(max_count);
        auto split = front.begin() + static_cast<std::ptrdiff_t>(max_count);
        std::move(front.begin(), split, std::back_inserter(batch));
        front.erase(front.begin(), split);
        return batch;
    }

    void wakeWaiter() {
        if (!m_hasWaiter) {
            return;
        }
        m_hasWaiter = false;
        auto handle = m_waiterHandle;
        m_waiterHandle = {};
        if (handle) {
            auto& waiter = handle.promise().coroutineRef();
            if (waiter.belongScheduler()) {
                waiter.resume();
                return;
            }
            handle.resume();
        }
    }

    std::deque<Batch> m_batches;
    bool m_closed = false;
    bool m_hasWaiter = false;
    std::coroutine_handle<Coroutine::promise_type> m_waiterHandle;
};

class Http2ConnContext {
public:
    using ActiveStreamBatch = std::optional<std::vector<Http2Stream::ptr>>;

    class GetActiveStreamsAwaitable {
    public:
        GetActiveStreamsAwaitable(Http2ConnContext* ctx, size_t max_count)
            : m_ctx(ctx)
            , m_recv_awaitable(ctx->m_mailbox->recvBatch(max_count))
        {
        }

        bool await_ready() const noexcept {
            return m_ctx->m_closed || m_recv_awaitable.await_ready();
        }

        template<typename Handle>
        bool await_suspend(Handle handle) {
            if (m_ctx->m_closed) {
                return false;
            }
            return m_recv_awaitable.await_suspend(handle);
        }

        ActiveStreamBatch await_resume() {
            if (m_ctx->m_closed) {
                return std::nullopt;
            }

            auto result = m_recv_awaitable.await_resume();
            if (!result) {
                m_ctx->m_closed = true;
                return std::nullopt;
            }
            return std::move(result.value());
        }

    private:
        Http2ConnContext* m_ctx;
        Http2ActiveStreamMailbox::RecvBatchAwaitable m_recv_awaitable;
    };

    explicit Http2ConnContext(Http2ActiveStreamMailbox& mailbox)
        : m_mailbox(&mailbox)
    {
    }

    Http2ConnContext(const Http2ConnContext&) = delete;
    Http2ConnContext& operator=(const Http2ConnContext&) = delete;
    Http2ConnContext(Http2ConnContext&&) = delete;
    Http2ConnContext& operator=(Http2ConnContext&&) = delete;

    auto getActiveStreams(
        size_t max_count = galay::kernel::UnsafeChannel<Http2Stream::ptr>::DEFAULT_BATCH_SIZE) {
        return GetActiveStreamsAwaitable(this, max_count);
    }

    bool isClosed() const {
        return m_closed;
    }

private:
    friend class GetActiveStreamsAwaitable;

    Http2ActiveStreamMailbox* m_mailbox;
    bool m_closed = false;
};

using Http2ActiveConnHandler = std::function<Coroutine(Http2ConnContext&)>;

/**
 * @brief HTTP/2 流管理器
 *
 * 职责：
 * 1. 运行帧读取协程（Reader），处理连接级帧，分发流级帧到对应 Http2Stream
 * 2. 运行帧写入协程（Writer），从连接内 send_queue 批量发送到 socket
 * 3. 新流创建后自动 spawn 用户 handler
 */
template<typename SocketType>
class Http2StreamManagerImpl
{
public:
    struct BackgroundLoops {
        Coroutine writer;
        Coroutine monitor;
    };

    class StartInBackgroundAwaitable
        : public galay::kernel::TimeoutSupport<StartInBackgroundAwaitable>
    {
    public:
        StartInBackgroundAwaitable(Http2StreamManagerImpl* manager, Http2StreamHandler handler)
            : m_manager(manager)
            , m_handler(std::move(handler))
        {
        }

        bool await_ready() const noexcept { return false; }

        template<typename Handle>
        bool await_suspend(Handle handle) {
            if (m_started) {
                return false;
            }
            m_started = true;
            auto* scheduler = handle.promise().getCoroutine().belongScheduler();
            if (scheduler) {
                m_manager->startWithScheduler(scheduler, std::move(m_handler));
            }
            return false;
        }

        StartInBackgroundAwaitable& wait() & { return *this; }
        StartInBackgroundAwaitable&& wait() && { return std::move(*this); }

        void await_resume() const noexcept {}

    private:
        Http2StreamManagerImpl* m_manager;
        Http2StreamHandler m_handler;
        bool m_started = false;
    };

    class ShutdownAwaitable
        : public galay::kernel::TimeoutSupport<ShutdownAwaitable>
    {
    public:
        ShutdownAwaitable(Http2StreamManagerImpl* manager, Http2ErrorCode error)
            : m_manager(manager)
            , m_error(error)
        {
        }

        bool await_ready() const noexcept { return false; }

        template<typename Handle>
        bool await_suspend(Handle handle) {
            if (!m_started) {
                m_started = true;
                m_wait_result.emplace(m_manager->shutdownImpl(m_error).wait());
            }
            return m_wait_result->await_suspend(handle);
        }

        ShutdownAwaitable& wait() & { return *this; }
        ShutdownAwaitable&& wait() && { return std::move(*this); }

        void await_resume() const noexcept {}

    private:
        Http2StreamManagerImpl* m_manager;
        Http2ErrorCode m_error;
        bool m_started = false;
        std::optional<galay::kernel::WaitResult> m_wait_result;
    };

    Http2StreamManagerImpl(Http2ConnImpl<SocketType>& conn)
        : m_conn(conn)
        , m_running(false)
        , m_send_channel(galay::kernel::UnsafeChannelWakeMode::Deferred)
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
        prepareForStart(false);
        auto loops = createBackgroundLoops();
        co_await startBackgroundLoops(loops).wait();
        co_await readerLoop(std::move(handler)).wait();
        co_await finishForegroundRun(loops).wait();
        co_return;
    }

    Coroutine start(Http2ActiveConnHandler handler) {
        prepareForStart(true);
        auto loops = createBackgroundLoops();
        co_await startBackgroundLoops(loops).wait();

        Http2ConnContext ctx(m_active_stream_mailbox);
        m_active_handlers.fetch_add(1, std::memory_order_acq_rel);
        Coroutine active_handler = runActiveHandler(std::move(handler), &ctx);
        co_await spawn(active_handler);

        co_await readerLoop(nullptr).wait();
        co_await finishForegroundRun(loops).wait();
        co_await active_handler.wait();
        co_return;
    }

    /**
     * @brief 将帧入队发送
     */
    void enqueueSendFrame(Http2Frame::uptr frame,
                          const Http2OutgoingFrame::WaiterPtr& waiter = nullptr) {
        m_send_channel.send(Http2OutgoingFrame{std::move(frame), waiter});
    }

    void enqueueSendBytes(std::string bytes,
                          const Http2OutgoingFrame::WaiterPtr& waiter = nullptr) {
        m_send_channel.send(Http2OutgoingFrame{std::move(bytes), waiter});
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
    StartInBackgroundAwaitable startInBackground(Http2StreamHandler handler) {
        return StartInBackgroundAwaitable(this, std::move(handler));
    }

    /**
     * @brief 从非协程上下文启动 StreamManager
     * @param scheduler 当前 IO 调度器
     * @param handler 用户流处理回调
     * @details 通过 scheduler->spawn() 启动 reader/writer/monitor，
     *          不需要协程上下文，可从 CustomAwaitable::await_resume() 等普通函数调用。
     */
    void startWithScheduler(galay::kernel::Scheduler* scheduler, Http2StreamHandler handler) {
        prepareForStart(false);
        scheduler->spawn(writerLoop());
        scheduler->spawn(monitorLoop());
        m_writer_ready.notify();
        scheduler->spawn(readerLoopThenCleanup(std::move(handler)));
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
    ShutdownAwaitable shutdown(Http2ErrorCode error = Http2ErrorCode::NoError) {
        return ShutdownAwaitable(this, error);
    }

private:
    Coroutine shutdownImpl(Http2ErrorCode error = Http2ErrorCode::NoError) {
        if (!m_started) co_return;

        if (m_running) {
            m_conn.setDraining(true);

            if (m_conn.isClient()) {
                m_reject_new_streams = true;
                auto waiter = sendGoaway(error);
                if (waiter) {
                    co_await waiter->wait();
                }
            } else {
                // RFC 推荐的 graceful shutdown：先发 MAX_INT，再发真实 last_stream_id。
                auto first = sendGoaway(error, "draining", kMaxStreamId);
                if (first) {
                    co_await first->wait();
                }

                auto rtt = m_conn.runtimeConfig().graceful_shutdown_rtt;
                if (rtt.count() > 0) {
                    co_await galay::kernel::sleep(rtt);
                }

                m_reject_new_streams = true;
                auto last_accepted = m_conn.lastPeerStreamId();
                auto second = sendGoaway(error, "", last_accepted);
                if (second) {
                    co_await second->wait();
                }
            }

            // 服务端等待活跃流处理完成，避免直接断开造成业务中断。
            auto deadline = std::chrono::steady_clock::now() + m_conn.runtimeConfig().graceful_shutdown_timeout;
            while (m_active_handlers.load(std::memory_order_acquire) > 0 &&
                   std::chrono::steady_clock::now() < deadline) {
                co_await galay::kernel::sleep(std::chrono::milliseconds(5));
            }

            // 批量发送 RST_STREAM 给所有未完成的流
            std::vector<Http2OutgoingFrame> rst_frames;
            m_conn.forEachStream([&](uint32_t stream_id, Http2Stream::ptr& stream) {
                if (stream && stream->state() != Http2StreamState::Closed) {
                    auto bytes = Http2FrameBuilder::rstStreamBytes(stream_id, Http2ErrorCode::NoError);
                    stream->onRstStreamSent();
                    rst_frames.push_back(Http2OutgoingFrame{std::move(bytes), nullptr});
                }
            });

            // 批量入队 RST_STREAM 帧
            for (auto& frame : rst_frames) {
                m_send_channel.send(std::move(frame));
            }

            // 等待 RST_STREAM 帧发送完成
            if (!rst_frames.empty()) {
                co_await galay::kernel::sleep(std::chrono::milliseconds(10));
            }

            // 关闭所有流的帧队列
            m_conn.forEachStream([](uint32_t, Http2Stream::ptr& stream) {
                stream->closeFrameQueue();
            });

            // 先只触发 transport shutdown，保留现有 awaitable，
            // 让 readerLoop 从 readFramesBatch() 正常收到 closing/peer-closed 并退出。
            m_conn.initiateClose();
            if (m_running) {
                co_await waitStopped();
            }
            // readerLoop/Writer 全部退出后再真正 close fd，避免提前移除底层 READ/CUSTOM awaitable。
            co_await m_conn.close();
        }
        co_return;
    }

    void prepareForStart(bool active_conn_mode) {
        m_started = true;
        m_running = true;
        m_active_conn_mode = active_conn_mode;
        m_active_stream_queue_closed = false;
        m_active_stream_mailbox.reset();
        m_draining_handlers.store(false, std::memory_order_release);
        m_reject_new_streams = false;
        m_last_frame_recv_at = std::chrono::steady_clock::now();
        m_waiting_ping_ack = false;

        if (m_next_local_stream_id == 0) {
            m_next_local_stream_id = m_conn.isClient() ? 3 : 2;
        }
        m_conn.reserveStreams(
            static_cast<size_t>(std::max<uint32_t>(m_conn.localSettings().max_concurrent_streams, 64u)) + 8);
    }

    BackgroundLoops createBackgroundLoops() {
        return BackgroundLoops{
            .writer = writerLoop(),
            .monitor = monitorLoop(),
        };
    }

    Coroutine startBackgroundLoops(BackgroundLoops& loops) {
        co_await spawn(loops.writer);
        co_await spawn(loops.monitor);
        m_writer_ready.notify();
        co_return;
    }

    Coroutine finishForegroundRun(BackgroundLoops& loops) {
        m_draining_handlers.store(true, std::memory_order_release);
        if (m_active_handlers.load(std::memory_order_acquire) > 0) {
            co_await m_handler_waiter.wait();
        }

        m_running = false;
        m_send_channel.send(Http2OutgoingFrame{});
        co_await loops.writer.wait();
        co_await loops.monitor.wait();
        m_stop_waiter.notify();
        co_return;
    }

    Coroutine readerLoopThenCleanup(Http2StreamHandler handler) {
        co_await readerLoop(std::move(handler)).wait();

        m_draining_handlers.store(true, std::memory_order_release);
        if (m_active_handlers.load(std::memory_order_acquire) > 0) {
            co_await m_handler_waiter.wait();
        }

        m_running = false;
        m_send_channel.send(Http2OutgoingFrame{});
        m_stop_waiter.notify();
        co_return;
    }

public:
    /**
     * @brief 发送 GOAWAY 帧
     * @return waiter，co_await waiter->wait() 等待发送完成
     */
    Http2OutgoingFrame::WaiterPtr sendGoaway(Http2ErrorCode error = Http2ErrorCode::NoError,
                                             const std::string& debug = "",
                                             std::optional<uint32_t> last_stream_id = std::nullopt) {
        auto waiter = std::make_shared<Http2OutgoingFrame::Waiter>();
        enqueueGoaway(error, debug, waiter, last_stream_id);
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
            if constexpr (!is_ssl_socket_v<SocketType>) {
                if (m_active_conn_mode && !m_conn.isClient()) {
                    auto frame_views_result = co_await m_conn.readFrameViewsBatch();

                    if (!frame_views_result) {
                        if (m_conn.isClosing() || m_conn.isPeerClosed()) {
                            HTTP_LOG_INFO("[stream-mgr] [reader] [exit] [{}] [{}]",
                                          m_conn.isPeerClosed() ? "peer-closed" : "closing",
                                          m_conn.lastReadError());
                            break;
                        }
                        if (frame_views_result.error() == Http2ErrorCode::NoError) {
                            continue;
                        }
                        if (frame_views_result.error() == Http2ErrorCode::ProtocolError &&
                            (m_conn.isPeerClosed() || m_conn.isClosing())) {
                            HTTP_LOG_INFO("[stream-mgr] [reader] [exit] [{}] [{}]",
                                          m_conn.isPeerClosed() ? "peer-closed" : "closing",
                                          m_conn.lastReadError());
                            break;
                        }
                        if (m_conn.lastReadError().empty()) {
                            HTTP_LOG_ERROR("[stream-mgr] [frame] [read-fail] [{}]",
                                          http2ErrorCodeToString(frame_views_result.error()));
                        } else {
                            HTTP_LOG_ERROR("[stream-mgr] [frame] [read-fail] [{}] [{}]",
                                          http2ErrorCodeToString(frame_views_result.error()),
                                          m_conn.lastReadError());
                        }
                        enqueueGoaway(frame_views_result.error());
                        break;
                    }

                    bool exit_loop = false;
                    auto& frame_views = *frame_views_result;
                    for (auto& frame_view : frame_views) {
                        const uint32_t stream_id = frame_view.streamId();
                        m_last_frame_recv_at = std::chrono::steady_clock::now();

                        HTTP_LOG_DEBUG("[stream-mgr] [frame] [recv-raw] [type={}] [stream={}] [flags=0x{:02x}]",
                                      http2FrameTypeToString(frame_view.header.type),
                                      stream_id,
                                      frame_view.header.flags);

                        if (m_conn.isExpectingContinuation()) {
                            if (!frame_view.isContinuation() ||
                                stream_id != m_conn.continuationStreamId()) {
                                enqueueGoaway(Http2ErrorCode::ProtocolError);
                                exit_loop = true;
                                break;
                            }
                        }

                        if (frame_view.isConnectionFrame()) {
                            auto frame = materializeFrameView(frame_view);
                            if (!frame) {
                                enqueueGoaway(frame.error());
                                exit_loop = true;
                                break;
                            }
                            handleConnectionFrame(std::move(*frame));
                            continue;
                        }

                        if (tryDispatchServerActiveFrameView(std::move(frame_view))) {
                            continue;
                        }

                        auto frame = materializeFrameView(frame_view);
                        if (!frame) {
                            enqueueGoaway(frame.error());
                            exit_loop = true;
                            break;
                        }
                        dispatchStreamFrame(std::move(*frame));
                    }

                    processPendingActions();
                    flushActiveStreams();

                    while (!m_pending_spawns.empty()) {
                        auto stream = m_pending_spawns.top();
                        m_pending_spawns.pop();
                        m_active_handlers.fetch_add(1, std::memory_order_acq_rel);
                        co_await spawn(runHandler(handler, stream));
                    }

                    if (exit_loop) {
                        break;
                    }
                    continue;
                }
            }

            auto frames_result = co_await m_conn.readFramesBatch();

            if (!frames_result) {
                if (m_conn.isClosing() || m_conn.isPeerClosed()) {
                    HTTP_LOG_INFO("[stream-mgr] [reader] [exit] [{}] [{}]",
                                  m_conn.isPeerClosed() ? "peer-closed" : "closing",
                                  m_conn.lastReadError());
                    break;
                }
                if (frames_result.error() == Http2ErrorCode::NoError) {
                    continue;
                }
                if (frames_result.error() == Http2ErrorCode::ProtocolError &&
                    (m_conn.isPeerClosed() || m_conn.isClosing())) {
                    HTTP_LOG_INFO("[stream-mgr] [reader] [exit] [{}] [{}]",
                                  m_conn.isPeerClosed() ? "peer-closed" : "closing",
                                  m_conn.lastReadError());
                    break;
                }
                if (m_conn.lastReadError().empty()) {
                    HTTP_LOG_ERROR("[stream-mgr] [frame] [read-fail] [{}]",
                                  http2ErrorCodeToString(frames_result.error()));
                } else {
                    HTTP_LOG_ERROR("[stream-mgr] [frame] [read-fail] [{}] [{}]",
                                  http2ErrorCodeToString(frames_result.error()),
                                  m_conn.lastReadError());
                }
                enqueueGoaway(frames_result.error());
                break;
            }

            bool exit_loop = false;
            auto& frames = *frames_result;
            for (auto& frame : frames) {
                uint32_t stream_id = frame->streamId();
                m_last_frame_recv_at = std::chrono::steady_clock::now();

                HTTP_LOG_DEBUG("[stream-mgr] [frame] [recv] [type={}] [stream={}] [flags=0x{:02x}]",
                              http2FrameTypeToString(frame->type()), stream_id, frame->header().flags);

                // CONTINUATION 状态检查
                if (m_conn.isExpectingContinuation()) {
                    if (!frame->isContinuation() || stream_id != m_conn.continuationStreamId()) {
                        enqueueGoaway(Http2ErrorCode::ProtocolError);
                        exit_loop = true;
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
            }

            // 处理 dispatchStreamFrame 中标记的待处理动作
            processPendingActions();
            flushActiveStreams();

            // spawn 待处理的流 handler
            while (!m_pending_spawns.empty()) {
                auto stream = m_pending_spawns.top();
                m_pending_spawns.pop();
                m_active_handlers.fetch_add(1, std::memory_order_acq_rel);
                co_await spawn(runHandler(handler, stream));
            }

            if (exit_loop) {
                break;
            }
        }
        // 关闭所有流的帧队列
        m_conn.forEachStream([](uint32_t, Http2Stream::ptr& stream) {
            stream->closeFrameQueue();
        });
        closeActiveStreamQueue();

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

    Coroutine runActiveHandler(Http2ActiveConnHandler handler, Http2ConnContext* ctx) {
        co_await handler(*ctx).wait();
        if (m_running && !m_conn.isClosing()) {
            m_conn.initiateClose();
        }
        int remaining = m_active_handlers.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0 && m_draining_handlers.load(std::memory_order_acquire)) {
            m_handler_waiter.notify();
        }
        co_return;
    }

    Coroutine monitorLoop() {
        while (m_running) {
            co_await galay::kernel::sleep(std::chrono::milliseconds(100));
            if (!m_running) {
                break;
            }

            auto now = std::chrono::steady_clock::now();

            const auto settings_timeout = m_conn.runtimeConfig().settings_ack_timeout;
            if (settings_timeout.count() > 0 &&
                m_conn.isSettingsAckPending() &&
                now - m_conn.settingsSentAt() > settings_timeout) {
                HTTP_LOG_WARN("[stream-mgr] [settings-timeout] [ack-missing]");
                enqueueGoaway(Http2ErrorCode::SettingsTimeout, "SETTINGS ACK timeout");
                m_conn.initiateClose();
                break;
            }

            if (!m_conn.runtimeConfig().ping_enabled ||
                m_conn.runtimeConfig().ping_interval.count() <= 0) {
                continue;
            }

            if (!m_waiting_ping_ack) {
                if (now - m_last_frame_recv_at >= m_conn.runtimeConfig().ping_interval) {
                    Http2PingFrame ping;
                    m_last_ping_payload.fill(0);
                    auto nonce = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            now.time_since_epoch()).count());
                    for (int i = 0; i < 8; ++i) {
                        m_last_ping_payload[7 - i] = static_cast<uint8_t>((nonce >> (i * 8)) & 0xFF);
                    }
                    ping.setOpaqueData(m_last_ping_payload.data());
                    enqueueSendFrame(std::move(ping));
                    m_waiting_ping_ack = true;
                    m_last_ping_sent_at = now;
                }
            } else if (m_conn.runtimeConfig().ping_timeout.count() > 0 &&
                       now - m_last_ping_sent_at > m_conn.runtimeConfig().ping_timeout) {
                HTTP_LOG_WARN("[stream-mgr] [ping-timeout] [ack-missing]");
                enqueueGoaway(Http2ErrorCode::ProtocolError, "PING ACK timeout");
                m_conn.initiateClose();
                break;
            }
        }
        co_return;
    }

    /**
     * @brief Writer 协程：从 send channel 接收数据并写入 socket
     * @details 使用 writev 批量发送多个帧，减少系统调用和内存拷贝
     */
    Coroutine writerLoop() {
        // 预分配待发送包和 iovec 数组，避免每次循环分配
        std::vector<Http2OutgoingFrame> outgoing_batch;
        std::vector<std::string> flattened_buffers;
        IoVecWriteState write_state;
        std::vector<Http2OutgoingFrame::WaiterPtr> waiters;
        outgoing_batch.reserve(64);
        flattened_buffers.reserve(64);
        write_state.reserve(64);
        waiters.reserve(64);

        while (true) {
            auto item_result = co_await m_send_channel.recv();
            if (!item_result) {
                HTTP_LOG_ERROR("[stream-mgr] [writer] [recv-fail]");
                break;
            }

            outgoing_batch.clear();
            flattened_buffers.clear();
            write_state.clear();
            waiters.clear();

            bool has_shutdown = false;
            auto collect_item = [&](Http2OutgoingFrame&& item) {
                if (item.isEmpty()) {
                    // 收到关闭信号，先发送已有数据再退出
                    HTTP_LOG_DEBUG("[stream-mgr] [writer] [shutdown]");
                    has_shutdown = true;
                    return;
                }
                if (item.waiter) {
                    waiters.push_back(std::move(item.waiter));
                }
                outgoing_batch.push_back(std::move(item));
            };

            collect_item(std::move(item_result.value()));

            while (!has_shutdown) {
                auto next = m_send_channel.tryRecv();
                if (!next.has_value()) {
                    break;
                }
                collect_item(std::move(next.value()));
            }

            if (!outgoing_batch.empty()) {
                flattened_buffers.reserve(outgoing_batch.size());
                write_state.reserve(outgoing_batch.size() * 2);
                for (auto& item : outgoing_batch) {
                    if (item.frame) {
                        flattened_buffers.push_back(item.frame->serialize());
                        auto& buffer = flattened_buffers.back();
                        write_state.append({
                            .iov_base = buffer.data(),
                            .iov_len = buffer.size()
                        });
                        continue;
                    }

                    std::array<struct iovec, 2> iovecs{};
                    const size_t count = item.exportIovecs(iovecs);
                    if (count == 0) {
                        continue;
                    }
                    for (size_t i = 0; i < count; ++i) {
                        write_state.append(iovecs[i]);
                    }
                }
            }

            if (!write_state.empty()) {
                if constexpr (requires(SocketType& socket, std::vector<iovec>& vec) { socket.writev(vec); }) {
                    // 支持 writev 的 socket（如 TcpSocket）：一次批量发送
                    while (!write_state.empty()) {
                        auto result = co_await m_conn.socket().writev(
                            std::span<const struct iovec>(write_state.data(), write_state.count()));
                        if (!result) {
                            if (m_conn.isClosing() || m_conn.isPeerClosed() ||
                                m_conn.isGoawaySent() || m_conn.isGoawayReceived()) {
                                HTTP_LOG_DEBUG("[stream-mgr] [writer] [writev-fail] [closing]");
                            } else {
                                HTTP_LOG_ERROR("[stream-mgr] [writer] [writev-fail]");
                            }
                            for (auto& waiter : waiters) {
                                if (waiter) {
                                    waiter->notify();
                                }
                            }
                            co_return;
                        }
                        const size_t written = result.value();
                        if (written == 0) {
                            HTTP_LOG_ERROR("[stream-mgr] [writer] [writev-zero]");
                            for (auto& waiter : waiters) {
                                if (waiter) {
                                    waiter->notify();
                                }
                            }
                            co_return;
                        }

                        if (write_state.advance(written) != written) {
                            HTTP_LOG_ERROR("[stream-mgr] [writer] [writev-advance-mismatch]");
                            for (auto& waiter : waiters) {
                                if (waiter) {
                                    waiter->notify();
                                }
                            }
                            co_return;
                        }
                }
            } else {
                    // 不支持 writev 的 socket（如 SslSocket）：按包串行 flatten 后 send
                    flattened_buffers.clear();
                    flattened_buffers.reserve(outgoing_batch.size());
                    for (const auto& item : outgoing_batch) {
                        flattened_buffers.push_back(item.flatten());
                    }
                    for (const auto& buffer : flattened_buffers) {
                        size_t offset = 0;
                        while (offset < buffer.size()) {
                            auto result = co_await m_conn.socket().send(buffer.data() + offset, buffer.size() - offset);
                            if (!result || result.value() == 0) {
                                if (m_conn.isClosing() || m_conn.isPeerClosed() ||
                                    m_conn.isGoawaySent() || m_conn.isGoawayReceived()) {
                                    HTTP_LOG_DEBUG("[stream-mgr] [writer] [send-fail] [closing]");
                                } else {
                                    HTTP_LOG_ERROR("[stream-mgr] [writer] [send-fail]");
                                }
                                for (auto& waiter : waiters) {
                                    if (waiter) {
                                        waiter->notify();
                                    }
                                }
                                co_return;
                            }
                            offset += result.value();
                        }
                    }
                }
            }

            // 通知所有 waiter
            for (auto& waiter : waiters) {
                if (waiter) {
                    waiter->notify();
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
                    m_conn.markSettingsAckReceived();
                    HTTP_LOG_DEBUG("[stream-mgr] [settings] [ack]");
                } else {
                    auto err = m_conn.peerSettings().applySettings(*settings);
                    if (err != Http2ErrorCode::NoError) {
                        enqueueGoaway(err);
                        return;
                    }
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
                } else if (m_waiting_ping_ack &&
                           std::memcmp(ping->opaqueData(), m_last_ping_payload.data(), 8) == 0) {
                    m_waiting_ping_ack = false;
                }
                break;
            }

            case Http2FrameType::GoAway: {
                auto* goaway = frame->asGoAway();
                m_reject_new_streams = true;
                m_conn.markGoawayReceived(
                    goaway->lastStreamId(), goaway->errorCode(), goaway->debugData());
                HTTP_LOG_INFO("[stream-mgr] [goaway] [recv] [last={}] [err={}] [debug={}]",
                             goaway->lastStreamId(),
                             http2ErrorCodeToString(goaway->errorCode()),
                             goaway->debugData());

                if (m_conn.isClient()) {
                    const uint32_t last = goaway->lastStreamId();
                    m_conn.forEachStream([&](uint32_t stream_id, Http2Stream::ptr& stream) {
                        if (!stream || stream_id <= last) {
                            return;
                        }
                        Http2GoAwayError err;
                        err.stream_id = stream_id;
                        err.last_stream_id = last;
                        err.error_code = goaway->errorCode();
                        err.retryable = true;
                        err.debug = goaway->debugData();
                        stream->setGoAwayError(std::move(err));
                        stream->closeFrameQueue();
                    });
                }
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
    void enqueueGoawayAction(Http2ErrorCode error) {
        m_pending_actions.push_back({PendingAction::Type::SendGoaway, 0, error});
    }

    void enqueueRstStreamAction(uint32_t stream_id, Http2ErrorCode error) {
        m_pending_actions.push_back({PendingAction::Type::SendRstStream, stream_id, error});
    }

    void enqueueWindowUpdateAction(uint32_t stream_id, uint32_t increment) {
        m_pending_actions.push_back({
            PendingAction::Type::SendWindowUpdate, stream_id, Http2ErrorCode::NoError, increment});
    }

    Http2Stream::ptr findAttachedStream(uint32_t stream_id) {
        if (m_hot_stream && m_hot_stream->streamId() == stream_id) {
            return m_hot_stream;
        }
        auto stream = m_conn.getStream(stream_id);
        attachStreamIO(stream);
        rememberHotStream(stream);
        return stream;
    }

    Http2Stream::ptr findOrCreateHeadersStream(uint32_t stream_id) {
        auto stream = findAttachedStream(stream_id);
        if (stream) {
            return stream;
        }

        if (m_conn.isClient()) {
            HTTP_LOG_WARN("[stream-mgr] [client] [headers] [unknown-stream={}]", stream_id);
            enqueueGoawayAction(Http2ErrorCode::ProtocolError);
            return nullptr;
        }

        if (m_reject_new_streams ||
            (m_conn.isGoawaySent() && m_conn.goawayLastStreamId() != kMaxStreamId)) {
            enqueueRstStreamAction(stream_id, Http2ErrorCode::RefusedStream);
            return nullptr;
        }
        if (stream_id <= m_conn.lastPeerStreamId()) {
            enqueueGoawayAction(Http2ErrorCode::ProtocolError);
            return nullptr;
        }
        if (m_conn.streamCount() >= m_conn.localSettings().max_concurrent_streams) {
            enqueueRstStreamAction(stream_id, Http2ErrorCode::RefusedStream);
            return nullptr;
        }

        stream = createStreamInternal(stream_id);
        m_conn.setLastPeerStreamId(stream_id);
        return stream;
    }

    void decodeBufferedHeaders(const Http2Stream::ptr& stream) {
        auto fields = m_conn.decoder().decode(stream->headerBlock());
        if (fields) {
            stream->setDecodedHeaders(std::move(fields.value()));
        }
        stream->clearHeaderBlock();
        m_conn.setExpectingContinuation(false);
    }

    void completeReceivedHeaders(const Http2Stream::ptr& stream, bool end_stream) {
        decodeBufferedHeaders(stream);

        if (m_conn.isClient()) {
            stream->consumeDecodedHeadersAsResponse();
            auto events = Http2StreamEvent::HeadersReady;
            if (end_stream) {
                stream->markResponseCompleted();
                events |= Http2StreamEvent::ResponseComplete;
            }
            markStreamActive(stream, events);
            return;
        }

        stream->consumeDecodedHeadersAsRequest();
        auto events = Http2StreamEvent::HeadersReady;
        if (end_stream) {
            stream->markRequestCompleted();
            events |= Http2StreamEvent::RequestComplete;
        }
        if (m_active_conn_mode) {
            markStreamActive(stream, events);
        } else {
            queueStreamHandler(stream);
        }
    }

    void applyRecvWindowUpdate(const Http2Stream::ptr& stream, uint32_t stream_id, size_t data_size) {
        auto update = m_conn.evaluateRecvWindowUpdate(stream->recvWindow(), data_size);
        if (update.conn_increment > 0) {
            enqueueWindowUpdateAction(0, update.conn_increment);
            m_conn.adjustConnRecvWindow(static_cast<int32_t>(update.conn_increment));
        }
        if (update.stream_increment > 0) {
            enqueueWindowUpdateAction(stream_id, update.stream_increment);
            stream->adjustRecvWindow(static_cast<int32_t>(update.stream_increment));
        }
    }

    void appendStreamDataAndMarkEvents(const Http2Stream::ptr& stream, Http2DataFrame* data) {
        auto events = Http2StreamEvent::DataArrived;
        if (m_conn.isClient()) {
            stream->appendResponseData(data->data());
            if (data->isEndStream()) {
                stream->markResponseCompleted();
                events |= Http2StreamEvent::ResponseComplete;
            }
        } else {
            if (m_active_conn_mode) {
                stream->appendRequestData(std::move(data->data()));
            } else {
                stream->appendRequestData(data->data());
            }
            if (data->isEndStream()) {
                stream->markRequestCompleted();
                events |= Http2StreamEvent::RequestComplete;
            }
        }
        markStreamActive(stream, events);
    }

    void appendStreamDataAndMarkEvents(const Http2Stream::ptr& stream,
                                       std::string_view data,
                                       bool end_stream) {
        auto events = Http2StreamEvent::DataArrived;
        if (m_conn.isClient()) {
            stream->appendResponseData(std::string(data));
            if (end_stream) {
                stream->markResponseCompleted();
                events |= Http2StreamEvent::ResponseComplete;
            }
        } else {
            stream->appendRequestData(data);
            if (end_stream) {
                stream->markRequestCompleted();
                events |= Http2StreamEvent::RequestComplete;
            }
        }
        markStreamActive(stream, events);
    }

    std::expected<Http2Frame::uptr, Http2ErrorCode> materializeFrameView(
        const Http2RawFrameView& frame_view) {
        auto bytes = frame_view.bytes();
        return Http2FrameParser::parseFrame(
            reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    }

    bool handleRawHeadersFrameView(const Http2RawFrameView& frame_view, uint32_t stream_id) {
        if ((frame_view.header.flags & Http2FrameFlags::kPadded) != 0 ||
            (frame_view.header.flags & Http2FrameFlags::kPriority) != 0) {
            return false;
        }

        auto stream = findOrCreateHeadersStream(stream_id);
        if (!stream) {
            return true;
        }

        if (!stream->canReceiveHeaders()) {
            enqueueRstStreamAction(stream_id, Http2ErrorCode::StreamClosed);
            return true;
        }

        const bool end_headers = frame_view.endHeaders();
        const bool end_stream = frame_view.endStream();
        stream->onHeadersReceived(end_stream);
        stream->appendHeaderBlock(frame_view.payload());

        if (end_headers) {
            completeReceivedHeaders(stream, end_stream);
        } else {
            m_conn.setExpectingContinuation(true, stream_id);
        }

        tryRetireClientStream(stream);
        return true;
    }

    bool handleRawContinuationFrameView(const Http2RawFrameView& frame_view, uint32_t stream_id) {
        auto stream = findAttachedStream(stream_id);
        if (!stream) {
            enqueueGoawayAction(Http2ErrorCode::ProtocolError);
            return true;
        }

        stream->appendHeaderBlock(frame_view.payload());

        if (frame_view.endHeaders()) {
            completeReceivedHeaders(stream, stream->isEndStreamReceived());
        }

        return true;
    }

    bool handleRawDataFrameView(const Http2RawFrameView& frame_view, uint32_t stream_id) {
        if ((frame_view.header.flags & Http2FrameFlags::kPadded) != 0) {
            return false;
        }

        if (stream_id == 0) {
            enqueueGoawayAction(Http2ErrorCode::ProtocolError);
            return true;
        }

        auto stream = findAttachedStream(stream_id);
        if (!stream) {
            enqueueRstStreamAction(stream_id, Http2ErrorCode::StreamClosed);
            return true;
        }
        if (!stream->canReceiveData()) {
            enqueueRstStreamAction(stream_id, Http2ErrorCode::StreamClosed);
            return true;
        }

        const auto payload = frame_view.payload();
        const int32_t data_size = static_cast<int32_t>(payload.size());
        stream->onDataReceived(frame_view.endStream());

        m_conn.adjustConnRecvWindow(-data_size);
        stream->adjustRecvWindow(-data_size);
        applyRecvWindowUpdate(stream, stream_id, payload.size());
        appendStreamDataAndMarkEvents(stream, payload, frame_view.endStream());

        tryRetireClientStream(stream);
        return true;
    }

    bool tryDispatchServerActiveFrameView(Http2RawFrameView&& frame_view) {
        if (!m_active_conn_mode || m_conn.isClient()) {
            return false;
        }

        const uint32_t stream_id = frame_view.streamId();

        if (frame_view.isHeaders()) {
            return handleRawHeadersFrameView(frame_view, stream_id);
        }

        if (frame_view.isContinuation()) {
            return handleRawContinuationFrameView(frame_view, stream_id);
        }

        if (frame_view.isData()) {
            return handleRawDataFrameView(frame_view, stream_id);
        }

        return false;
    }

    void handleHeadersFrame(Http2Frame::uptr frame, uint32_t stream_id) {
        auto stream = findOrCreateHeadersStream(stream_id);
        if (!stream) {
            return;
        }

        if (!stream->canReceiveHeaders()) {
            enqueueRstStreamAction(stream_id, Http2ErrorCode::StreamClosed);
            return;
        }

        auto* hdrs = frame->asHeaders();
        if (hdrs->hasPriority()) {
            stream->setPriority(hdrs->exclusive(), hdrs->streamDependency(), hdrs->weight());
        }

        const bool end_headers = hdrs->isEndHeaders();
        const bool end_stream = hdrs->isEndStream();
        stream->onHeadersReceived(end_stream);
        stream->appendHeaderBlock(hdrs->headerBlock());

        if (end_headers) {
            completeReceivedHeaders(stream, end_stream);
        } else {
            m_conn.setExpectingContinuation(true, stream_id);
        }

        pushStreamFrameIfNeeded(stream, std::move(frame));
        tryRetireClientStream(stream);
    }

    void handleContinuationFrame(Http2Frame::uptr frame, uint32_t stream_id) {
        auto stream = findAttachedStream(stream_id);
        if (!stream) {
            enqueueGoawayAction(Http2ErrorCode::ProtocolError);
            return;
        }

        auto* cont = frame->asContinuation();
        stream->appendHeaderBlock(cont->headerBlock());

        if (cont->isEndHeaders()) {
            completeReceivedHeaders(stream, stream->isEndStreamReceived());
        }

        pushStreamFrameIfNeeded(stream, std::move(frame));
    }

    void handleDataFrame(Http2Frame::uptr frame, uint32_t stream_id) {
        if (stream_id == 0) {
            enqueueGoawayAction(Http2ErrorCode::ProtocolError);
            return;
        }

        auto stream = findAttachedStream(stream_id);
        if (!stream) {
            enqueueRstStreamAction(stream_id, Http2ErrorCode::StreamClosed);
            return;
        }
        if (!stream->canReceiveData()) {
            enqueueRstStreamAction(stream_id, Http2ErrorCode::StreamClosed);
            return;
        }

        auto* data = frame->asData();
        const int32_t data_size = static_cast<int32_t>(data->data().size());
        stream->onDataReceived(data->isEndStream());

        m_conn.adjustConnRecvWindow(-data_size);
        stream->adjustRecvWindow(-data_size);
        applyRecvWindowUpdate(stream, stream_id, data->data().size());
        appendStreamDataAndMarkEvents(stream, data);

        pushStreamFrameIfNeeded(stream, std::move(frame));
        tryRetireClientStream(stream);
    }

    void handlePriorityFrame(Http2Frame::uptr frame, uint32_t stream_id) {
        auto stream = findAttachedStream(stream_id);
        if (!stream) {
            return;
        }

        auto* prio = frame->asPriority();
        stream->setPriority(prio->exclusive(), prio->streamDependency(), prio->weight());
    }

    void handleRstStreamFrame(Http2Frame::uptr frame, uint32_t stream_id) {
        if (stream_id == 0) {
            enqueueGoawayAction(Http2ErrorCode::ProtocolError);
            return;
        }

        auto stream = findAttachedStream(stream_id);
        if (!stream) {
            return;
        }

        stream->onRstStreamReceived();
        HTTP_LOG_DEBUG("[stream-mgr] [stream] [rst] [id={}] [err={}]",
                      stream_id, http2ErrorCodeToString(frame->asRstStream()->errorCode()));
        markStreamActive(stream, Http2StreamEvent::Reset);
        pushStreamFrameIfNeeded(stream, std::move(frame));
        stream->markRequestCompleted();
        stream->markResponseCompleted();
        stream->closeFrameQueue();
        tryRetireClientStream(stream);
    }

    void handleWindowUpdateFrame(Http2Frame::uptr frame, uint32_t stream_id) {
        auto stream = findAttachedStream(stream_id);
        if (!stream) {
            return;
        }

        auto* wu = frame->asWindowUpdate();
        const uint32_t increment = wu->windowSizeIncrement();
        if (increment == 0) {
            enqueueRstStreamAction(stream_id, Http2ErrorCode::ProtocolError);
            return;
        }

        stream->adjustSendWindow(increment);
        markStreamActive(stream, Http2StreamEvent::WindowUpdated);
        pushStreamFrameIfNeeded(stream, std::move(frame));
    }

    void handlePushPromiseFrame(Http2Frame::uptr frame, uint32_t stream_id) {
        if (!m_conn.isClient()) {
            enqueueGoawayAction(Http2ErrorCode::ProtocolError);
            return;
        }

        auto* pp = frame->asPushPromise();
        const uint32_t promised_id = pp->promisedStreamId();
        auto promised_stream = findAttachedStream(promised_id);
        if (!promised_stream) {
            promised_stream = createStreamInternal(promised_id);
            promised_stream->setState(Http2StreamState::ReservedRemote);
        }

        pushStreamFrameIfNeeded(promised_stream, std::move(frame));
        if (!m_active_conn_mode) {
            queueStreamHandler(promised_stream);
        }
    }

    void dispatchStreamFrame(Http2Frame::uptr frame) {
        const uint32_t stream_id = frame->streamId();

        if (stream_id == 0) {
            return;
        }

        if (frame->isHeaders()) {
            handleHeadersFrame(std::move(frame), stream_id);
            return;
        }

        if (frame->isContinuation()) {
            handleContinuationFrame(std::move(frame), stream_id);
            return;
        }

        if (frame->isData()) {
            handleDataFrame(std::move(frame), stream_id);
            return;
        }

        if (frame->isPriority()) {
            handlePriorityFrame(std::move(frame), stream_id);
            return;
        }

        if (frame->isRstStream()) {
            handleRstStreamFrame(std::move(frame), stream_id);
            return;
        }

        if (frame->isWindowUpdate()) {
            handleWindowUpdateFrame(std::move(frame), stream_id);
            return;
        }

        if (frame->isPushPromise()) {
            handlePushPromiseFrame(std::move(frame), stream_id);
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
            m_pending_actions.pop_front();

            switch (action.type) {
                case PendingAction::Type::SendGoaway: {
                    enqueueGoaway(action.error_code);
                    break;
                }
                case PendingAction::Type::SendRstStream: {
                    auto bytes = Http2FrameBuilder::rstStreamBytes(action.stream_id, action.error_code);
                    auto stream = m_conn.getStream(action.stream_id);
                    if (stream) {
                        stream->onRstStreamSent();
                    }
                    enqueueSendBytes(std::move(bytes));
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
                       const Http2OutgoingFrame::WaiterPtr& waiter = nullptr,
                       std::optional<uint32_t> last_stream_id = std::nullopt) {
        Http2GoAwayFrame frame;
        uint32_t last = last_stream_id.value_or(m_conn.lastPeerStreamId());
        frame.setLastStreamId(last);
        frame.setErrorCode(error);
        if (!debug.empty()) {
            frame.setDebugData(debug);
        }
        m_conn.markGoawaySent(last, error, debug);
        enqueueSendFrame(std::move(frame), waiter);
    }

    /**
     * @brief 将新流加入待 spawn 队列
     */
    void queueStreamHandler(Http2Stream::ptr stream) {
        m_pending_spawns.push(stream);
    }

    void markStreamActive(const Http2Stream::ptr& stream, Http2StreamEvent events) {
        if (!m_active_conn_mode) {
            return;
        }
        m_active_batch.mark(stream, events);
    }

    void closeActiveStreamQueue() {
        if (!m_active_conn_mode || m_active_stream_queue_closed) {
            return;
        }
        m_active_stream_queue_closed = true;
        m_active_stream_mailbox.close();
    }

    void pushStreamFrameIfNeeded(const Http2Stream::ptr& stream, Http2Frame::uptr frame) {
        if (!stream) {
            return;
        }
        if (m_active_conn_mode) {
            return;
        }
        stream->pushFrame(std::move(frame));
    }

    void flushActiveStreams() {
        if (!m_active_conn_mode || m_active_batch.empty()) {
            return;
        }

        auto ready = m_active_batch.takeReady();
        m_active_stream_mailbox.sendBatch(std::move(ready));
    }

    void tryRetireClientStream(const Http2Stream::ptr& stream) {
        if (!stream || !m_conn.isClient()) {
            return;
        }
        if (!stream->isResponseCompleted()) {
            return;
        }
        if (stream->state() != Http2StreamState::Closed) {
            return;
        }
        m_conn.removeStream(stream->streamId());
    }

    Http2Stream::ptr createStreamInternal(uint32_t stream_id) {
        Http2Stream::ptr stream;
        if constexpr (!is_ssl_socket_v<SocketType>) {
            if (m_active_conn_mode && !m_conn.isClient()) {
                stream = m_conn.createStream(stream_id, m_stream_pool.acquire(stream_id));
            } else {
                stream = m_conn.createStream(stream_id);
            }
        } else {
            stream = m_conn.createStream(stream_id);
        }
        attachStreamIO(stream);
        rememberHotStream(stream);
        return stream;
    }

    void attachStreamIO(const Http2Stream::ptr& stream) {
        if (!stream) return;
        auto* encoder = &m_conn.encoder();
        auto* decoder = &m_conn.decoder();
        if (stream->m_io_attached &&
            stream->m_send_channel == &m_send_channel &&
            stream->m_encoder == encoder &&
            stream->m_decoder == decoder) {
            return;
        }
        stream->attachIO(&m_send_channel, encoder, decoder);
        if (m_active_conn_mode && !m_conn.isClient()) {
            stream->setRetireCallback([this](uint32_t stream_id) {
                clearHotStream(stream_id);
                m_conn.removeStream(stream_id);
            });
        } else {
            stream->setRetireCallback(nullptr);
        }
    }

    void rememberHotStream(const Http2Stream::ptr& stream) {
        if (!stream || !m_active_conn_mode || m_conn.isClient()) {
            return;
        }
        m_hot_stream = stream;
    }

    void clearHotStream(uint32_t stream_id) {
        if (m_hot_stream && m_hot_stream->streamId() == stream_id) {
            m_hot_stream.reset();
        }
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
    Http2Stream::ptr m_hot_stream;
    bool m_reject_new_streams = false;
    std::chrono::steady_clock::time_point m_last_frame_recv_at{};
    std::chrono::steady_clock::time_point m_last_ping_sent_at{};
    std::array<uint8_t, 8> m_last_ping_payload{};
    bool m_waiting_ping_ack = false;
    bool m_active_conn_mode = false;
    bool m_active_stream_queue_closed = false;
    Http2ActiveStreamBatch m_active_batch;
    Http2ActiveStreamMailbox m_active_stream_mailbox;
    Http2StreamPool m_stream_pool;

    // 发送通道：空指针表示关闭信号
    UnsafeChannel<Http2OutgoingFrame> m_send_channel;

    // 待处理动作队列
    std::deque<PendingAction> m_pending_actions;

    // 待 spawn 的流队列（按优先级排序）
    std::priority_queue<Http2Stream::ptr, std::vector<Http2Stream::ptr>, StreamPriorityCompare> m_pending_spawns;
};

// 类型别名
using Http2StreamManager = Http2StreamManagerImpl<galay::async::TcpSocket>;

} // namespace galay::http2

#endif // GALAY_HTTP2_STREAM_MANAGER_H
