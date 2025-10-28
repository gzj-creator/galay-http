#ifndef GALAY_HTTP2_WRITER_H
#define GALAY_HTTP2_WRITER_H

#include <galay/kernel/async/Socket.h>
#include <galay/kernel/async/TimerGenerator.h>
#include <galay/kernel/coroutine/AsyncWaiter.hpp>
#include "galay-http/protoc/http2/Http2Frame.h"
#include "galay-http/protoc/http2/Http2Error.h"
#include "Http2Params.hpp"
#include "Http2Stream.h"

namespace galay::http
{
    /**
     * @brief HTTP/2 写入器
     * 
     * 类似于 WsWriter，负责发送 HTTP/2 帧
     */
    class Http2Writer
    {
    public:
        Http2Writer(AsyncTcpSocket& socket, TimerGenerator& generator, 
                   Http2StreamManager& stream_manager, const Http2Settings& params);
        
        // ==================== 连接级别的帧 ====================
        
        /// 发送连接前言（客户端）
        AsyncResult<std::expected<void, Http2Error>>
            sendPreface(std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));
        
        /// 发送 SETTINGS 帧
        AsyncResult<std::expected<void, Http2Error>>
            sendSettings(const Http2Settings& settings, 
                        std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));
        
        /// 发送 SETTINGS ACK
        AsyncResult<std::expected<void, Http2Error>>
            sendSettingsAck(std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));
        
        /// 发送 PING 帧
        AsyncResult<std::expected<void, Http2Error>>
            sendPing(uint64_t data = 0, bool ack = false,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));
        
        /// 发送 GOAWAY 帧
        AsyncResult<std::expected<void, Http2Error>>
            sendGoAway(uint32_t last_stream_id, Http2ErrorCode error_code, 
                      const std::string& debug_data = "",
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));
        
        /// 发送 WINDOW_UPDATE 帧（连接级别）
        AsyncResult<std::expected<void, Http2Error>>
            sendConnectionWindowUpdate(uint32_t increment,
                                      std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));
        
        // ==================== 流级别的帧 ====================
        
        /// 发送 HEADERS 帧
        AsyncResult<std::expected<void, Http2Error>>
            sendHeaders(uint32_t stream_id, const std::string& header_block,
                       bool end_stream = false, bool end_headers = true,
                       std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));
        
        /// 发送 DATA 帧
        AsyncResult<std::expected<void, Http2Error>>
            sendData(uint32_t stream_id, const std::string& data, bool end_stream = false,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));
        
        /// 发送 RST_STREAM 帧
        AsyncResult<std::expected<void, Http2Error>>
            sendRstStream(uint32_t stream_id, Http2ErrorCode error_code,
                         std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));
        
        /// 发送 WINDOW_UPDATE 帧（流级别）
        AsyncResult<std::expected<void, Http2Error>>
            sendStreamWindowUpdate(uint32_t stream_id, uint32_t increment,
                                  std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));
        
        /// 发送 PRIORITY 帧
        AsyncResult<std::expected<void, Http2Error>>
            sendPriority(uint32_t stream_id, uint32_t dependency, 
                        uint8_t weight, bool exclusive = false,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));
        
        // ==================== 通用帧发送 ====================
        
        /// 发送任意帧
        AsyncResult<std::expected<void, Http2Error>>
            sendFrame(const Http2Frame& frame,
                     std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));
        
    private:
        Coroutine<nil> sendFrameInternal(
            std::string data,
            std::shared_ptr<AsyncWaiter<void, Http2Error>> waiter,
            std::chrono::milliseconds timeout);
        
        Coroutine<nil> sendData(
            const std::string& data,
            std::shared_ptr<AsyncWaiter<void, Http2Error>> waiter,
            std::chrono::milliseconds timeout);
        
    private:
        AsyncTcpSocket& m_socket;
        Http2Settings m_params;
        TimerGenerator& m_generator;
        Http2StreamManager& m_stream_manager;
    };
}

#endif // GALAY_HTTP2_WRITER_H

