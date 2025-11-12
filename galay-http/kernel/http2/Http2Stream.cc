#include "Http2Stream.h"
#include "galay-http/utils/Http2DebugLog.h"
#include "Http2Connection.h"

namespace galay::http
{
    // ==================== Http2Stream ====================
    
    Http2Stream::Http2Stream(uint32_t stream_id, uint32_t initial_window_size, Http2Connection& connection)
        : m_stream_id(stream_id)
        , m_state(Http2StreamState::IDLE)
        , m_send_window_size(initial_window_size)
        , m_recv_window_size(initial_window_size)
        , m_dependency(0)
        , m_weight(16)  // 默认权重
        , m_exclusive(false)
        , m_error_code(Http2ErrorCode::NO_ERROR)
        , m_connection(&connection)
    {
        HTTP2_LOG_DEBUG("[Http2Stream] Created stream {}, initial window: {}", stream_id, initial_window_size);
    }

    Http2Connection& Http2Stream::connection()
    {
        return *m_connection;
    }
    
    bool Http2Stream::canSendData() const
    {
        return m_state == Http2StreamState::OPEN || 
               m_state == Http2StreamState::HALF_CLOSED_REMOTE;
    }
    
    bool Http2Stream::canReceiveData() const
    {
        return m_state == Http2StreamState::OPEN || 
               m_state == Http2StreamState::HALF_CLOSED_LOCAL;
    }
    
    std::expected<void, Http2Error> Http2Stream::updateSendWindow(uint32_t increment)
    {
        int64_t new_size = static_cast<int64_t>(m_send_window_size) + increment;
        
        // 检查溢出（窗口大小不能超过 2^31 - 1）
        if (new_size > 0x7FFFFFFF) {
            HTTP2_LOG_ERROR("[Http2Stream] Stream {} send window overflow: {} + {}", 
                           m_stream_id, m_send_window_size, increment);
            return std::unexpected(Http2Error(kHttp2Error_FlowControlError));
        }
        
        m_send_window_size = static_cast<int32_t>(new_size);
        HTTP2_LOG_DEBUG("[Http2Stream] Stream {} send window updated: {} (increment: {})", 
                       m_stream_id, m_send_window_size, increment);
        return {};
    }
    
    std::expected<void, Http2Error> Http2Stream::updateRecvWindow(uint32_t increment)
    {
        int64_t new_size = static_cast<int64_t>(m_recv_window_size) + increment;
        
        if (new_size > 0x7FFFFFFF) {
            HTTP2_LOG_ERROR("[Http2Stream] Stream {} recv window overflow: {} + {}", 
                           m_stream_id, m_recv_window_size, increment);
            return std::unexpected(Http2Error(kHttp2Error_FlowControlError));
        }
        
        m_recv_window_size = static_cast<int32_t>(new_size);
        HTTP2_LOG_DEBUG("[Http2Stream] Stream {} recv window updated: {} (increment: {})", 
                       m_stream_id, m_recv_window_size, increment);
        return {};
    }
    
    std::expected<void, Http2Error> Http2Stream::consumeSendWindow(uint32_t size)
    {
        if (static_cast<int32_t>(size) > m_send_window_size) {
            HTTP2_LOG_ERROR("[Http2Stream] Stream {} send window exhausted: need {}, have {}", 
                           m_stream_id, size, m_send_window_size);
            return std::unexpected(Http2Error(kHttp2Error_FlowControlError));
        }
        
        m_send_window_size -= size;
        HTTP2_LOG_DEBUG("[Http2Stream] Stream {} send window consumed: {} (remaining: {})", 
                       m_stream_id, size, m_send_window_size);
        return {};
    }
    
    std::expected<void, Http2Error> Http2Stream::consumeRecvWindow(uint32_t size)
    {
        if (static_cast<int32_t>(size) > m_recv_window_size) {
            HTTP2_LOG_ERROR("[Http2Stream] Stream {} recv window exhausted: need {}, have {}", 
                           m_stream_id, size, m_recv_window_size);
            return std::unexpected(Http2Error(kHttp2Error_FlowControlError));
        }
        
        m_recv_window_size -= size;
        HTTP2_LOG_DEBUG("[Http2Stream] Stream {} recv window consumed: {} (remaining: {})", 
                       m_stream_id, size, m_recv_window_size);
        return {};
    }
    
    void Http2Stream::appendReceivedData(const std::string& data)
    {
        m_received_data += data;
        HTTP2_LOG_DEBUG("[Http2Stream] Stream {} received {} bytes (total: {})", 
                       m_stream_id, data.size(), m_received_data.size());
    }
    
    std::string Http2Stream::getReceivedData()
    {
        std::string data = std::move(m_received_data);
        m_received_data.clear();
        return data;
    }
    
    void Http2Stream::clearReceivedData()
    {
        m_received_data.clear();
    }
    
    void Http2Stream::setHeaders(const std::string& header_block)
    {
        m_headers = header_block;
        HTTP2_LOG_DEBUG("[Http2Stream] Stream {} headers set: {} bytes", 
                       m_stream_id, header_block.size());
    }
    
    void Http2Stream::setPriority(uint32_t dependency, uint8_t weight, bool exclusive)
    {
        m_dependency = dependency;
        m_weight = weight;
        m_exclusive = exclusive;
        HTTP2_LOG_DEBUG("[Http2Stream] Stream {} priority: dep={}, weight={}, exclusive={}", 
                       m_stream_id, dependency, weight, exclusive);
        // 注意：优先级队列的更新应该在 Http2StreamManager 中处理
        // 这里只更新流本身的优先级信息
    }
    
    void Http2Stream::setError(Http2ErrorCode error_code)
    {
        m_error_code = error_code;
        HTTP2_LOG_ERROR("[Http2Stream] Stream {} error: {}", 
                       m_stream_id, http2ErrorCodeToString(error_code));
    }
    
    // ==================== Http2StreamManager ====================
    
    Http2StreamManager::Http2StreamManager(Http2Connection& connection, const Http2Settings& settings)
        : m_is_server(false)
        , m_next_stream_id(1)  // 客户端从 1 开始
        , m_max_concurrent_streams(settings.max_concurrent_streams)
        , m_initial_window_size(settings.initial_window_size)
        , m_connection_send_window(settings.connection_window_size)
        , m_connection_recv_window(settings.connection_window_size)
        , m_priority_queue_dirty(false)
        , m_connection(&connection)
    {
        HTTP2_LOG_DEBUG("[Http2StreamManager] Created, max_streams={}, initial_window={}", 
                       m_max_concurrent_streams, m_initial_window_size);
    }
    
    std::expected<Http2Stream::ptr, Http2Error> Http2StreamManager::createStream(uint32_t stream_id)
    {
        // 检查流是否已存在
        if (m_streams.find(stream_id) != m_streams.end()) {
            HTTP2_LOG_ERROR("[Http2StreamManager] Stream {} already exists", stream_id);
            return std::unexpected(Http2Error(kHttp2Error_ProtocolError));
        }
        
        // 检查并发流数量限制
        if (m_streams.size() >= m_max_concurrent_streams) {
            HTTP2_LOG_ERROR("[Http2StreamManager] Too many streams: {}/{}", 
                           m_streams.size(), m_max_concurrent_streams);
            return std::unexpected(Http2Error(kHttp2Error_TooManyStreams));
        }
        
        // 检查流 ID 的奇偶性
        bool client_initiated = (stream_id % 2) == 1;
        if (m_is_server && client_initiated) {
            // 服务器接收客户端发起的流（奇数）
        } else if (!m_is_server && !client_initiated) {
            // 客户端接收服务器发起的流（偶数）
        } else if (m_is_server && !client_initiated) {
            // 服务器发起的流必须是偶数
        } else if (!m_is_server && client_initiated) {
            // 客户端发起的流必须是奇数
        }
        
        auto stream = std::make_shared<Http2Stream>(stream_id, m_initial_window_size, *m_connection);
        m_streams[stream_id] = stream;
        
        // 添加到优先级队列
        m_priority_queue.push(stream);
        
        HTTP2_LOG_INFO("[Http2StreamManager] Created stream {}, active: {}/{}", 
                      stream_id, m_streams.size(), m_max_concurrent_streams);
        
        return stream;
    }
    
    Http2Stream::ptr Http2StreamManager::getStream(uint32_t stream_id)
    {
        auto it = m_streams.find(stream_id);
        if (it == m_streams.end()) {
            return nullptr;
        }
        return it->second;
    }
    
    void Http2StreamManager::removeStream(uint32_t stream_id)
    {
        auto it = m_streams.find(stream_id);
        if (it != m_streams.end()) {
            HTTP2_LOG_INFO("[Http2StreamManager] Removed stream {}, active: {}", 
                          stream_id, m_streams.size() - 1);
            m_streams.erase(it);
            // 标记优先级队列需要重建（因为priority_queue不支持删除特定元素）
            m_priority_queue_dirty = true;
        }
    }
    
    void Http2StreamManager::closeStream(uint32_t stream_id)
    {
        auto stream = getStream(stream_id);
        if (stream) {
            stream->setState(Http2StreamState::CLOSED);
            HTTP2_LOG_DEBUG("[Http2StreamManager] Closed stream {}", stream_id);
            
            // 可以选择立即删除或延迟删除
            // removeStream(stream_id);
        }
    }
    
    uint32_t Http2StreamManager::getNextStreamId()
    {
        uint32_t id = m_next_stream_id;
        m_next_stream_id += 2;  // 客户端奇数，服务器偶数
        return id;
    }
    
    std::expected<void, Http2Error> Http2StreamManager::updateConnectionSendWindow(uint32_t increment)
    {
        int64_t new_size = static_cast<int64_t>(m_connection_send_window) + increment;
        
        if (new_size > 0x7FFFFFFF) {
            HTTP2_LOG_ERROR("[Http2StreamManager] Connection send window overflow: {} + {}", 
                           m_connection_send_window, increment);
            return std::unexpected(Http2Error(kHttp2Error_FlowControlError));
        }
        
        m_connection_send_window = static_cast<int32_t>(new_size);
        HTTP2_LOG_DEBUG("[Http2StreamManager] Connection send window updated: {} (increment: {})", 
                       m_connection_send_window, increment);
        return {};
    }
    
    std::expected<void, Http2Error> Http2StreamManager::updateConnectionRecvWindow(uint32_t increment)
    {
        int64_t new_size = static_cast<int64_t>(m_connection_recv_window) + increment;
        
        if (new_size > 0x7FFFFFFF) {
            HTTP2_LOG_ERROR("[Http2StreamManager] Connection recv window overflow: {} + {}", 
                           m_connection_recv_window, increment);
            return std::unexpected(Http2Error(kHttp2Error_FlowControlError));
        }
        
        m_connection_recv_window = static_cast<int32_t>(new_size);
        HTTP2_LOG_DEBUG("[Http2StreamManager] Connection recv window updated: {} (increment: {})", 
                       m_connection_recv_window, increment);
        return {};
    }
    
    std::expected<void, Http2Error> Http2StreamManager::consumeConnectionSendWindow(uint32_t size)
    {
        if (static_cast<int32_t>(size) > m_connection_send_window) {
            HTTP2_LOG_ERROR("[Http2StreamManager] Connection send window exhausted: need {}, have {}", 
                           size, m_connection_send_window);
            return std::unexpected(Http2Error(kHttp2Error_FlowControlError));
        }
        
        m_connection_send_window -= size;
        HTTP2_LOG_DEBUG("[Http2StreamManager] Connection send window consumed: {} (remaining: {})", 
                       size, m_connection_send_window);
        return {};
    }
    
    std::expected<void, Http2Error> Http2StreamManager::consumeConnectionRecvWindow(uint32_t size)
    {
        if (static_cast<int32_t>(size) > m_connection_recv_window) {
            HTTP2_LOG_ERROR("[Http2StreamManager] Connection recv window exhausted: need {}, have {}", 
                           size, m_connection_recv_window);
            return std::unexpected(Http2Error(kHttp2Error_FlowControlError));
        }
        
        m_connection_recv_window -= size;
        HTTP2_LOG_DEBUG("[Http2StreamManager] Connection recv window consumed: {} (remaining: {})", 
                       size, m_connection_recv_window);
        return {};
    }
    
    void Http2StreamManager::updateSettings(const Http2Settings& settings)
    {
        m_max_concurrent_streams = settings.max_concurrent_streams;
        m_initial_window_size = settings.initial_window_size;
        
        HTTP2_LOG_INFO("[Http2StreamManager] Settings updated: max_streams={}, initial_window={}", 
                      m_max_concurrent_streams, m_initial_window_size);
    }
    
    // ==================== 优先级调度 ====================
    
    bool Http2StreamManager::StreamPriorityComparator::operator()(
        const Http2Stream::ptr& a, const Http2Stream::ptr& b) const
    {
        // priority_queue 是最大堆，但我们希望优先级高的（值小的）在顶部
        // 所以这里返回 true 表示 a 的优先级低于 b（a 应该在 b 后面）
        
        // 1. 首先比较依赖关系：依赖的流优先级更高（dependency=0 表示根流，优先级最高）
        if (a->dependency() != b->dependency()) {
            // dependency 越小，优先级越高（0 是根流）
            return a->dependency() > b->dependency();
        }
        
        // 2. 如果依赖相同，比较权重：权重越大，优先级越高
        // 但 priority_queue 是最大堆，所以需要反转
        if (a->weight() != b->weight()) {
            return a->weight() < b->weight();
        }
        
        // 3. 如果权重也相同，exclusive 的流优先级更高
        if (a->exclusive() != b->exclusive()) {
            return !a->exclusive();  // exclusive 的优先级更高
        }
        
        // 4. 最后按 stream_id 排序（小的优先级更高）
        return a->streamId() > b->streamId();
    }
    
    uint64_t Http2StreamManager::calculatePriorityValue(const Http2Stream::ptr& stream) const
    {
        // 计算优先级值，用于排序
        // 值越小，优先级越高
        
        uint64_t value = 0;
        
        // dependency 越小，优先级越高（0 是根流）
        value |= (static_cast<uint64_t>(stream->dependency()) << 32);
        
        // 权重越大，优先级越高（反转：256 - weight）
        value |= (static_cast<uint64_t>(256 - stream->weight()) << 24);
        
        // exclusive 的优先级更高
        if (!stream->exclusive()) {
            value |= (1ULL << 16);
        }
        
        // stream_id 越小，优先级越高
        value |= stream->streamId();
        
        return value;
    }
    
    void Http2StreamManager::rebuildPriorityQueue()
    {
        // 清空现有队列
        m_priority_queue = std::priority_queue<Http2Stream::ptr, 
                                               std::vector<Http2Stream::ptr>, 
                                               StreamPriorityComparator>();
        
        // 重新添加所有流
        for (const auto& [stream_id, stream] : m_streams) {
            m_priority_queue.push(stream);
        }
        
        m_priority_queue_dirty = false;
        HTTP2_LOG_DEBUG("[Http2StreamManager] Priority queue rebuilt with {} streams", m_streams.size());
    }
    
    Http2Stream::ptr Http2StreamManager::getNextStreamToSchedule()
    {
        // 如果队列需要重建，先重建
        if (m_priority_queue_dirty) {
            rebuildPriorityQueue();
        }
        
        // 从优先级队列中获取最高优先级的流
        // 但需要确保该流仍然存在且可以调度
        while (!m_priority_queue.empty()) {
            auto stream = m_priority_queue.top();
            m_priority_queue.pop();
            
            // 检查流是否仍然存在且可以发送数据
            if (m_streams.find(stream->streamId()) != m_streams.end() && 
                stream->canSendData() && 
                !stream->isClosed()) {
                return stream;
            }
        }
        
        return nullptr;
    }
    
    void Http2StreamManager::updateStreamPriority(uint32_t stream_id)
    {
        auto it = m_streams.find(stream_id);
        if (it != m_streams.end()) {
            // 标记需要重建优先级队列
            m_priority_queue_dirty = true;
            HTTP2_LOG_DEBUG("[Http2StreamManager] Stream {} priority updated, queue marked dirty", stream_id);
        }
    }
}

