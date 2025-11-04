#include "Http2Reader.h"
#include "galay-http/utils/Http2DebugLog.h"

namespace galay::http
{
    Http2Reader::Http2Reader(Http2SocketAdapter socket, TimerGenerator& generator,
                            Http2StreamManager& stream_manager, Http2Settings params)
        : m_socket(socket)
        , m_generator(generator)
        , m_stream_manager(stream_manager)
        , m_params(params)
        , m_buffer(params.max_frame_size + HTTP2_FRAME_HEADER_SIZE)
    {
        HTTP2_LOG_DEBUG("[Http2Reader] Created with max_frame_size={}", params.max_frame_size);
    }
    
    AsyncResult<std::expected<Http2Frame::ptr, Http2Error>>
    Http2Reader::readFrame(std::chrono::milliseconds timeout)
    {
        HTTP2_LOG_DEBUG("[Http2Reader] Reading frame");
        
        if (timeout.count() == -1) {
            timeout = m_params.recv_timeout;
        }
        
        auto waiter = std::make_shared<AsyncWaiter<Http2Frame::ptr, Http2Error>>();
        auto co = readFrameInternal(waiter, timeout);
        waiter->appendTask(std::move(co));
        
        return waiter->wait();
    }
    
    AsyncResult<std::expected<void, Http2Error>>
    Http2Reader::readPreface(std::chrono::milliseconds timeout)
    {
        if (timeout.count() == -1) {
            timeout = m_params.recv_timeout;
        }

        auto waiter = std::make_shared<AsyncWaiter<void, Http2Error>>();
        auto co = readPrefaceInternal(waiter, timeout);
        waiter->appendTask(std::move(co));
        
        return waiter->wait();
    }
    
    Coroutine<nil> Http2Reader::readFrameInternal(
        std::shared_ptr<AsyncWaiter<Http2Frame::ptr, Http2Error>> waiter,
        std::chrono::milliseconds timeout)
    {
        size_t recv_size = 0;
        
        // 读取帧头（9字节）
        while (recv_size < HTTP2_FRAME_HEADER_SIZE) {
            std::expected<Bytes, CommonError> bytes;
            if (timeout < std::chrono::milliseconds(0)) {
                bytes = co_await m_socket.recv(m_buffer.data() + recv_size, 
                                              m_buffer.capacity() - recv_size);
            } else {
                auto res = co_await m_generator.timeout<std::expected<Bytes, CommonError>>([&, this](){
                    return m_socket.recv(m_buffer.data() + recv_size, 
                                        m_buffer.capacity() - recv_size);
                }, timeout);
                if (!res) {
                    HTTP2_LOG_ERROR("[Http2Reader] Failed to read frame header: timeout");
                    waiter->notify(std::unexpected(Http2Error(kHttp2Error_ConnectionTimeout)));
                    co_return nil();
                }
                bytes = std::move(res.value());
            }
            
            if (!bytes || bytes.value().empty()) {
                HTTP2_LOG_ERROR("[Http2Reader] Failed to read frame header: connection closed");
                waiter->notify(std::unexpected(Http2Error(kHttp2Error_ConnectionClosed)));
                co_return nil();
            }
            
            recv_size += bytes.value().size();
        }
        
        // 解析帧头
        auto header_result = Http2FrameHeader::deserialize(
            reinterpret_cast<const uint8_t*>(m_buffer.data()), 
            recv_size
        );
        
        if (!header_result.has_value()) {
            waiter->notify(std::unexpected(header_result.error()));
            co_return nil();
        }
        
        auto& header = header_result.value();
        
        HTTP2_LOG_DEBUG("[Http2Reader] Read frame header: type={}, stream={}, length={}", 
                       http2FrameTypeToString(header.type), header.stream_id, header.length);
        
        // 检查帧大小
        if (header.length > m_params.max_frame_size) {
            HTTP2_LOG_ERROR("[Http2Reader] Frame too large: length={}, max={}", 
                           header.length, m_params.max_frame_size);
            waiter->notify(std::unexpected(Http2Error(kHttp2Error_FrameTooLarge)));
            co_return nil();
        }
        
        // 读取帧负载
        size_t total_frame_size = HTTP2_FRAME_HEADER_SIZE + header.length;
        while (recv_size < total_frame_size) {
            std::expected<Bytes, CommonError> bytes;
            if (timeout < std::chrono::milliseconds(0)) {
                bytes = co_await m_socket.recv(m_buffer.data() + recv_size, 
                                              m_buffer.capacity() - recv_size);
            } else {
                auto res = co_await m_generator.timeout<std::expected<Bytes, CommonError>>([&, this](){
                    return m_socket.recv(m_buffer.data() + recv_size, 
                                        m_buffer.capacity() - recv_size);
                }, timeout);
                if (!res) {
                    HTTP2_LOG_ERROR("[Http2Reader] Failed to read frame payload: timeout");
                    waiter->notify(std::unexpected(Http2Error(kHttp2Error_ConnectionTimeout)));
                    co_return nil();
                }
                bytes = std::move(res.value());
            }
            
            if (!bytes || bytes.value().empty()) {
                HTTP2_LOG_ERROR("[Http2Reader] Failed to read frame payload: connection closed");
                waiter->notify(std::unexpected(Http2Error(kHttp2Error_ConnectionClosed)));
                co_return nil();
            }
            
            recv_size += bytes.value().size();
        }
        
        // 创建帧对象
        auto frame_result = Http2Frame::createFrame(header);
        if (!frame_result.has_value()) {
            waiter->notify(std::unexpected(frame_result.error()));
            co_return nil();
        }
        
        auto frame = frame_result.value();
        
        // 反序列化负载
        if (header.length > 0) {
            auto payload_result = frame->deserializePayload(
                reinterpret_cast<const uint8_t*>(m_buffer.data() + HTTP2_FRAME_HEADER_SIZE),
                header.length
            );
            
            if (!payload_result.has_value()) {
                waiter->notify(std::unexpected(payload_result.error()));
                co_return nil();
            }
        }
        
        HTTP2_LOG_DEBUG("[Http2Reader] Frame read successfully");
        waiter->notify(frame);
        co_return nil();
    }
    
    Coroutine<nil> Http2Reader::readPrefaceInternal(
        std::shared_ptr<AsyncWaiter<void, Http2Error>> waiter,
        std::chrono::milliseconds timeout)
    {
        size_t recv_size = 0;
        
        // 读取 24 字节连接前言
        while (recv_size < HTTP2_CONNECTION_PREFACE_LENGTH) {
            std::expected<Bytes, CommonError> bytes;
            if (timeout < std::chrono::milliseconds(0)) {
                bytes = co_await m_socket.recv(m_buffer.data() + recv_size, 
                                              m_buffer.capacity() - recv_size);
            } else {
                auto res = co_await m_generator.timeout<std::expected<Bytes, CommonError>>([&, this](){
                    return m_socket.recv(m_buffer.data() + recv_size, 
                                        m_buffer.capacity() - recv_size);
                }, timeout);
                if (!res) {
                    HTTP2_LOG_ERROR("[Http2Reader] Failed to read connection preface: timeout");
                    waiter->notify(std::unexpected(Http2Error(kHttp2Error_ConnectionTimeout)));
                    co_return nil();
                }
                bytes = std::move(res.value());
            }
            
            if (!bytes || bytes.value().empty()) {
                HTTP2_LOG_ERROR("[Http2Reader] Failed to read connection preface: connection closed");
                waiter->notify(std::unexpected(Http2Error(kHttp2Error_ConnectionClosed)));
                co_return nil();
            }
            
            recv_size += bytes.value().size();
        }
        
        // 验证前言
        if (std::memcmp(m_buffer.data(), HTTP2_CONNECTION_PREFACE, HTTP2_CONNECTION_PREFACE_LENGTH) != 0) {
            HTTP2_LOG_ERROR("[Http2Reader] Invalid connection preface");
            waiter->notify(std::unexpected(Http2Error(kHttp2Error_InvalidPreface)));
            co_return nil();
        }
        
        HTTP2_LOG_INFO("[Http2Reader] Connection preface received");
        
        waiter->notify({});
        co_return nil();
    }
}
