#include "Http2Writer.h"
#include "galay-http/utils/Http2DebugLog.h"
#include "galay/kernel/coroutine/CoSchedulerHandle.hpp"
#include "galay/kernel/async/AsyncFactory.h"
#include <galay/common/Base.h>

namespace galay::http
{
    Http2Writer::Http2Writer(Http2SocketAdapter socket, CoSchedulerHandle handle,
                            Http2StreamManager& stream_manager, const Http2Settings& params)
        : m_socket(socket)
        , m_params(params)
        , m_handle(handle)
        , m_stream_manager(stream_manager)
    {
        HTTP2_LOG_DEBUG("[Http2Writer] Created");
    }
    
    // ==================== 连接前言 ====================
    
    AsyncResult<std::expected<void, Http2Error>>
    Http2Writer::sendPreface(std::chrono::milliseconds timeout)
    {
        HTTP2_LOG_DEBUG("[Http2Writer] Sending connection preface");
        
        if (timeout.count() == -1) {
            timeout = m_params.send_timeout;
        }
        
        auto waiter = std::make_shared<AsyncWaiter<void, Http2Error>>();
        
        // 发送连接前言
        std::string preface(HTTP2_CONNECTION_PREFACE, HTTP2_CONNECTION_PREFACE_LENGTH);
        auto co = sendData(preface, waiter, timeout);
        waiter->appendTask(std::move(co));
        
        return waiter->wait();
    }
    
    // ==================== SETTINGS ====================
    
    AsyncResult<std::expected<void, Http2Error>>
    Http2Writer::sendSettings(const Http2Settings& settings, std::chrono::milliseconds timeout)
    {
        HTTP2_LOG_DEBUG("[Http2Writer] Sending SETTINGS");
        
        if (timeout.count() == -1) {
            timeout = m_params.send_timeout;
        }
        
        Http2SettingsFrame frame(false);
        frame.setSetting(Http2SettingsId::HEADER_TABLE_SIZE, settings.header_table_size);
        frame.setSetting(Http2SettingsId::ENABLE_PUSH, settings.enable_push ? 1 : 0);
        frame.setSetting(Http2SettingsId::MAX_CONCURRENT_STREAMS, settings.max_concurrent_streams);
        frame.setSetting(Http2SettingsId::INITIAL_WINDOW_SIZE, settings.initial_window_size);
        frame.setSetting(Http2SettingsId::MAX_FRAME_SIZE, settings.max_frame_size);
        frame.setSetting(Http2SettingsId::MAX_HEADER_LIST_SIZE, settings.max_header_list_size);
        
        return sendFrame(frame, timeout);
    }
    
    AsyncResult<std::expected<void, Http2Error>>
    Http2Writer::sendSettingsAck(std::chrono::milliseconds timeout)
    {
        HTTP2_LOG_DEBUG("[Http2Writer] Sending SETTINGS ACK");
        
        if (timeout.count() == -1) {
            timeout = m_params.send_timeout;
        }
        
        Http2SettingsFrame frame(true);  // ACK
        return sendFrame(frame, timeout);
    }
    
    // ==================== PING ====================
    
    AsyncResult<std::expected<void, Http2Error>>
    Http2Writer::sendPing(uint64_t data, bool ack, std::chrono::milliseconds timeout)
    {
        HTTP2_LOG_DEBUG("[Http2Writer] Sending PING, ack={}", ack);
        
        if (timeout.count() == -1) {
            timeout = m_params.send_timeout;
        }
        
        Http2PingFrame frame(data, ack);
        return sendFrame(frame, timeout);
    }
    
    // ==================== GOAWAY ====================
    
    AsyncResult<std::expected<void, Http2Error>>
    Http2Writer::sendGoAway(uint32_t last_stream_id, Http2ErrorCode error_code,
                           const std::string& debug_data, std::chrono::milliseconds timeout)
    {
        HTTP2_LOG_INFO("[Http2Writer] Sending GOAWAY, last_stream={}, error={}", 
                      last_stream_id, http2ErrorCodeToString(error_code));
        
        if (timeout.count() == -1) {
            timeout = m_params.send_timeout;
        }
        
        Http2GoAwayFrame frame(last_stream_id, error_code, debug_data);
        return sendFrame(frame, timeout);
    }
    
    // ==================== WINDOW_UPDATE ====================
    
    AsyncResult<std::expected<void, Http2Error>>
    Http2Writer::sendConnectionWindowUpdate(uint32_t increment, std::chrono::milliseconds timeout)
    {
        HTTP2_LOG_DEBUG("[Http2Writer] Sending connection WINDOW_UPDATE, increment={}", increment);
        
        if (timeout.count() == -1) {
            timeout = m_params.send_timeout;
        }
        
        Http2WindowUpdateFrame frame(0, increment);  // stream_id = 0 表示连接级别
        return sendFrame(frame, timeout);
    }
    
    AsyncResult<std::expected<void, Http2Error>>
    Http2Writer::sendStreamWindowUpdate(uint32_t stream_id, uint32_t increment, 
                                       std::chrono::milliseconds timeout)
    {
        HTTP2_LOG_DEBUG("[Http2Writer] Sending stream {} WINDOW_UPDATE, increment={}", 
                       stream_id, increment);
        
        if (timeout.count() == -1) {
            timeout = m_params.send_timeout;
        }
        
        Http2WindowUpdateFrame frame(stream_id, increment);
        return sendFrame(frame, timeout);
    }
    
    // ==================== HEADERS ====================
    
    AsyncResult<std::expected<void, Http2Error>>
    Http2Writer::sendHeaders(uint32_t stream_id, const std::string& header_block,
                            bool end_stream, bool end_headers, std::chrono::milliseconds timeout)
    {
        HTTP2_LOG_DEBUG("[Http2Writer] Sending HEADERS for stream {}, size={}, end_stream={}, end_headers={}", 
                       stream_id, header_block.size(), end_stream, end_headers);
        
        if (timeout.count() == -1) {
            timeout = m_params.send_timeout;
        }
        
        // 如果头部块大小超过 max_frame_size，需要分片
        if (header_block.size() > m_params.max_frame_size) {
            HTTP2_LOG_DEBUG("[Http2Writer] Header block size {} exceeds max_frame_size {}, splitting into CONTINUATION frames", 
                           header_block.size(), m_params.max_frame_size);
            
            // 发送第一个 HEADERS 帧（不设置 END_HEADERS）
            std::string first_chunk = header_block.substr(0, m_params.max_frame_size);
            Http2HeadersFrame first_frame(stream_id, first_chunk, end_stream, false);
            
            auto waiter = std::make_shared<AsyncWaiter<void, Http2Error>>();
            auto co = sendHeadersWithContinuation(stream_id, header_block, end_stream, waiter, timeout);
            waiter->appendTask(std::move(co));
            return waiter->wait();
        }
        
        Http2HeadersFrame frame(stream_id, header_block, end_stream, end_headers);
        return sendFrame(frame, timeout);
    }
    
    // ==================== DATA ====================
    
    AsyncResult<std::expected<void, Http2Error>>
    Http2Writer::sendData(uint32_t stream_id, const std::string& data, 
                         bool end_stream, std::chrono::milliseconds timeout)
    {
        HTTP2_LOG_DEBUG("[Http2Writer] Sending DATA for stream {}, size={}, end_stream={}", 
                       stream_id, data.size(), end_stream);
        
        if (timeout.count() == -1) {
            timeout = m_params.send_timeout;
        }
        
        // 检查流控窗口
        auto stream = m_stream_manager.getStream(stream_id);
        if (!stream) {
            HTTP2_LOG_ERROR("[Http2Writer] Stream {} not found", stream_id);
            return AsyncResult<std::expected<void, Http2Error>>(
                std::unexpected(Http2Error(kHttp2Error_StreamNotFound))
            );
        }
        
        // 检查流级别窗口
        if (static_cast<int32_t>(data.size()) > stream->sendWindowSize()) {
            HTTP2_LOG_ERROR("[Http2Writer] Stream {} send window exhausted: need {}, have {}", 
                           stream_id, data.size(), stream->sendWindowSize());
            return AsyncResult<std::expected<void, Http2Error>>(
                std::unexpected(Http2Error(kHttp2Error_FlowControlError))
            );
        }
        
        // 检查连接级别窗口
        if (static_cast<int32_t>(data.size()) > m_stream_manager.connectionSendWindow()) {
            HTTP2_LOG_ERROR("[Http2Writer] Connection send window exhausted: need {}, have {}", 
                           data.size(), m_stream_manager.connectionSendWindow());
            return AsyncResult<std::expected<void, Http2Error>>(
                std::unexpected(Http2Error(kHttp2Error_FlowControlError))
            );
        }
        
        // 消耗窗口
        auto stream_result = stream->consumeSendWindow(data.size());
        if (!stream_result) {
            return AsyncResult<std::expected<void, Http2Error>>(
                std::unexpected(stream_result.error())
            );
        }
        
        auto conn_result = m_stream_manager.consumeConnectionSendWindow(data.size());
        if (!conn_result) {
            return AsyncResult<std::expected<void, Http2Error>>(
                std::unexpected(conn_result.error())
            );
        }
        
        // 如果数据大小超过 max_frame_size，需要分片
        if (data.size() > m_params.max_frame_size) {
            HTTP2_LOG_DEBUG("[Http2Writer] Data size {} exceeds max_frame_size {}, splitting into multiple DATA frames", 
                           data.size(), m_params.max_frame_size);
            
            auto waiter = std::make_shared<AsyncWaiter<void, Http2Error>>();
            auto co = sendDataChunked(stream_id, data, end_stream, waiter, timeout);
            waiter->appendTask(std::move(co));
            return waiter->wait();
        }
        
        Http2DataFrame frame(stream_id, data, end_stream);
        return sendFrame(frame, timeout);
    }
    
    // ==================== RST_STREAM ====================
    
    AsyncResult<std::expected<void, Http2Error>>
    Http2Writer::sendRstStream(uint32_t stream_id, Http2ErrorCode error_code, 
                              std::chrono::milliseconds timeout)
    {
        HTTP2_LOG_INFO("[Http2Writer] Sending RST_STREAM for stream {}, error={}", 
                      stream_id, http2ErrorCodeToString(error_code));
        
        if (timeout.count() == -1) {
            timeout = m_params.send_timeout;
        }
        
        Http2RstStreamFrame frame(stream_id, error_code);
        return sendFrame(frame, timeout);
    }
    
    // ==================== PRIORITY ====================
    
    AsyncResult<std::expected<void, Http2Error>>
    Http2Writer::sendPriority(uint32_t stream_id, uint32_t dependency,
                             uint8_t weight, bool exclusive, std::chrono::milliseconds timeout)
    {
        HTTP2_LOG_DEBUG("[Http2Writer] Sending PRIORITY for stream {}, dep={}, weight={}, exclusive={}", 
                       stream_id, dependency, weight, exclusive);
        
        if (timeout.count() == -1) {
            timeout = m_params.send_timeout;
        }
        
        Http2PriorityFrame frame(stream_id, dependency, weight, exclusive);
        return sendFrame(frame, timeout);
    }
    
    // ==================== 通用帧发送 ====================
    
    AsyncResult<std::expected<void, Http2Error>>
    Http2Writer::sendFrame(const Http2Frame& frame, std::chrono::milliseconds timeout)
    {
        HTTP2_LOG_DEBUG("[Http2Writer] Sending frame type={}, stream={}, length={}", 
                       http2FrameTypeToString(frame.type()), 
                       frame.streamId(), 
                       frame.length());
        
        if (timeout.count() == -1) {
            timeout = m_params.send_timeout;
        }
        
        auto waiter = std::make_shared<AsyncWaiter<void, Http2Error>>();
        
        // 序列化帧
        std::string data = frame.serialize();
        
        auto co = sendFrameInternal(std::move(data), waiter, timeout);
        waiter->appendTask(std::move(co));
        
        return waiter->wait();
    }
    
    // ==================== 内部实现 ====================
    
    Coroutine<nil> Http2Writer::sendFrameInternal(
        std::string data,
        std::shared_ptr<AsyncWaiter<void, Http2Error>> waiter,
        std::chrono::milliseconds timeout)
    {
        // 转换为 Bytes
        auto bytes = Bytes::fromString(data);
        auto generator = m_handle.getAsyncFactory().getTimerGenerator();
        while (true) {
            std::expected<Bytes, CommonError> res;
            if (timeout < std::chrono::milliseconds(0)) {
                res = co_await m_socket.send(std::move(bytes));
            } else {
                auto temp = co_await generator.timeout<std::expected<Bytes, CommonError>>([&](){
                    return m_socket.send(std::move(bytes));
                }, timeout);
                if (!temp) {
                    HTTP2_LOG_ERROR("[Http2Writer] Send timeout");
                    waiter->notify(std::unexpected(Http2Error(kHttp2Error_SendTimeout)));
                    co_return nil();
                }
                res = std::move(temp.value());
            }
            
            if (!res) {
                HTTP2_LOG_ERROR("[Http2Writer] Send error: {}", res.error().message());
                waiter->notify(std::unexpected(Http2Error(kHttp2Error_SendError, res.error().message())));
                co_return nil();
            }
            
            // 更新 bytes 并检查是否全部发送完成
            bytes = std::move(res.value());
            if (bytes.empty()) {
                HTTP2_LOG_DEBUG("[Http2Writer] Send completed");
                break;
            }
        }
        
        waiter->notify({});
        co_return nil();
    }
    
    Coroutine<nil> Http2Writer::sendData(
        const std::string& data,
        std::shared_ptr<AsyncWaiter<void, Http2Error>> waiter,
        std::chrono::milliseconds timeout)
    {
        // 转换为 Bytes
        auto bytes = Bytes::fromString(data);
        auto generator = m_handle.getAsyncFactory().getTimerGenerator();
        while (true) {
            std::expected<Bytes, CommonError> res;
            if (timeout < std::chrono::milliseconds(0)) {
                res = co_await m_socket.send(std::move(bytes));
            } else {
                auto temp = co_await generator.timeout<std::expected<Bytes, CommonError>>([&](){
                    return m_socket.send(std::move(bytes));
                }, timeout);
                if (!temp) {
                    HTTP2_LOG_ERROR("[Http2Writer] Send timeout");
                    waiter->notify(std::unexpected(Http2Error(kHttp2Error_SendTimeout)));
                    co_return nil();
                }
                res = std::move(temp.value());
            }
            
            if (!res) {
                HTTP2_LOG_ERROR("[Http2Writer] Send error: {}", res.error().message());
                waiter->notify(std::unexpected(Http2Error(kHttp2Error_SendError, res.error().message())));
                co_return nil();
            }
            
            // 更新 bytes 并检查是否全部发送完成
            bytes = std::move(res.value());
            if (bytes.empty()) {
                HTTP2_LOG_DEBUG("[Http2Writer] Send completed");
                break;
            }
        }
        
        waiter->notify({});
        co_return nil();
    }
    
    Coroutine<nil> Http2Writer::sendDataChunked(
        uint32_t stream_id,
        const std::string& data,
        bool end_stream,
        std::shared_ptr<AsyncWaiter<void, Http2Error>> waiter,
        std::chrono::milliseconds timeout)
    {
        HTTP2_LOG_DEBUG("[Http2Writer] Sending chunked DATA for stream {}, total size={}", stream_id, data.size());
        
        size_t offset = 0;
        size_t remaining = data.size();
        int chunk_count = 0;
        
        while (remaining > 0) {
            size_t chunk_size = std::min(remaining, static_cast<size_t>(m_params.max_frame_size));
            bool is_last_chunk = (remaining == chunk_size);
            bool chunk_end_stream = is_last_chunk && end_stream;
            
            std::string chunk = data.substr(offset, chunk_size);
            Http2DataFrame frame(stream_id, chunk, chunk_end_stream);
            
            chunk_count++;
            HTTP2_LOG_DEBUG("[Http2Writer] Sending DATA chunk #{} for stream {}, size={}, end_stream={}", 
                           chunk_count, stream_id, chunk_size, chunk_end_stream);
            
            // 序列化并发送帧
            std::string frame_data = frame.serialize();
            auto send_waiter = std::make_shared<AsyncWaiter<void, Http2Error>>();
            auto send_co = sendFrameInternal(std::move(frame_data), send_waiter, timeout);
            send_waiter->appendTask(std::move(send_co));
            auto result = co_await send_waiter->wait();
            
            if (!result) {
                HTTP2_LOG_ERROR("[Http2Writer] Failed to send DATA chunk #{}: {}", chunk_count, result.error().message());
                waiter->notify(std::unexpected(result.error()));
                co_return nil();
            }
            
            offset += chunk_size;
            remaining -= chunk_size;
        }
        
        HTTP2_LOG_INFO("[Http2Writer] Successfully sent {} DATA chunks for stream {}", chunk_count, stream_id);
        waiter->notify({});
        co_return nil();
    }
    
    Coroutine<nil> Http2Writer::sendHeadersWithContinuation(
        uint32_t stream_id,
        const std::string& header_block,
        bool end_stream,
        std::shared_ptr<AsyncWaiter<void, Http2Error>> waiter,
        std::chrono::milliseconds timeout)
    {
        HTTP2_LOG_DEBUG("[Http2Writer] Sending HEADERS with CONTINUATION for stream {}, total size={}", 
                       stream_id, header_block.size());
        
        size_t offset = 0;
        size_t remaining = header_block.size();
        bool first_frame = true;
        int frame_count = 0;
        
        while (remaining > 0) {
            size_t chunk_size = std::min(remaining, static_cast<size_t>(m_params.max_frame_size));
            bool is_last_chunk = (remaining == chunk_size);
            
            std::string chunk = header_block.substr(offset, chunk_size);
            
            if (first_frame) {
                // 第一个帧使用 HEADERS
                Http2HeadersFrame frame(stream_id, chunk, end_stream, is_last_chunk);
                
                frame_count++;
                HTTP2_LOG_DEBUG("[Http2Writer] Sending HEADERS frame for stream {}, size={}, end_headers={}", 
                               stream_id, chunk_size, is_last_chunk);
                
                std::string frame_data = frame.serialize();
                auto send_waiter = std::make_shared<AsyncWaiter<void, Http2Error>>();
                auto send_co = sendFrameInternal(std::move(frame_data), send_waiter, timeout);
                send_waiter->appendTask(std::move(send_co));
                auto result = co_await send_waiter->wait();
                
                if (!result) {
                    HTTP2_LOG_ERROR("[Http2Writer] Failed to send HEADERS frame: {}", result.error().message());
                    waiter->notify(std::unexpected(result.error()));
                    co_return nil();
                }
                
                first_frame = false;
            } else {
                // 后续帧使用 CONTINUATION
                Http2ContinuationFrame frame(stream_id, chunk, is_last_chunk);
                
                frame_count++;
                HTTP2_LOG_DEBUG("[Http2Writer] Sending CONTINUATION frame #{} for stream {}, size={}, end_headers={}", 
                               frame_count, stream_id, chunk_size, is_last_chunk);
                
                std::string frame_data = frame.serialize();
                auto send_waiter = std::make_shared<AsyncWaiter<void, Http2Error>>();
                auto send_co = sendFrameInternal(std::move(frame_data), send_waiter, timeout);
                send_waiter->appendTask(std::move(send_co));
                auto result = co_await send_waiter->wait();
                
                if (!result) {
                    HTTP2_LOG_ERROR("[Http2Writer] Failed to send CONTINUATION frame: {}", result.error().message());
                    waiter->notify(std::unexpected(result.error()));
                    co_return nil();
                }
            }
            
            offset += chunk_size;
            remaining -= chunk_size;
        }
        
        HTTP2_LOG_INFO("[Http2Writer] Successfully sent {} frame(s) (HEADERS + CONTINUATION) for stream {}", 
                      frame_count, stream_id);
        waiter->notify({});
        co_return nil();
    }
}

