#include "Http2Stream.h"
#include "galay-http/utils/Http2DebugLog.h"

namespace galay::http
{
    // ==================== Http2Stream ====================
    
    Http2Stream::Http2Stream(uint32_t stream_id, uint32_t initial_window_size)
        : m_stream_id(stream_id)
        , m_state(Http2StreamState::IDLE)
        , m_send_window_size(initial_window_size)
        , m_recv_window_size(initial_window_size)
        , m_dependency(0)
        , m_weight(16)  // 默认权重
        , m_exclusive(false)
        , m_error_code(Http2ErrorCode::NO_ERROR)
    {
        HTTP2_LOG_DEBUG("[Http2Stream] Created stream {}, initial window: {}", stream_id, initial_window_size);
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
    }
    
    void Http2Stream::setError(Http2ErrorCode error_code)
    {
        m_error_code = error_code;
        HTTP2_LOG_ERROR("[Http2Stream] Stream {} error: {}", 
                       m_stream_id, http2ErrorCodeToString(error_code));
    }
    
    // ==================== Http2StreamManager ====================
    
    Http2StreamManager::Http2StreamManager(const Http2Settings& settings)
        : m_is_server(false)
        , m_next_stream_id(1)  // 客户端从 1 开始
        , m_max_concurrent_streams(settings.max_concurrent_streams)
        , m_initial_window_size(settings.initial_window_size)
        , m_connection_send_window(settings.connection_window_size)
        , m_connection_recv_window(settings.connection_window_size)
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
        
        auto stream = std::make_shared<Http2Stream>(stream_id, m_initial_window_size);
        m_streams[stream_id] = stream;
        
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
}

