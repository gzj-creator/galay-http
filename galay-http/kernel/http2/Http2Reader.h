#ifndef GALAY_HTTP2_READER_H
#define GALAY_HTTP2_READER_H

#include <galay/kernel/async/Socket.h>
#include <galay/kernel/async/TimerGenerator.h>
#include <galay/kernel/coroutine/AsyncWaiter.hpp>
#include "galay-http/protoc/http2/Http2Frame.h"
#include "galay-http/protoc/http2/Http2Error.h"
#include "Http2Params.hpp"
#include "Http2Stream.h"
#include "Http2SocketAdapter.h"
#include <galay/common/Buffer.h>

namespace galay::http
{
    /**
     * @brief HTTP/2 读取器
     * 
     * 类似于 WsReader，负责读取和解析 HTTP/2 帧
     * 支持 AsyncTcpSocket 和 AsyncSslSocket 通过 Http2SocketAdapter
     */
    class Http2Reader
    {
    public:
        Http2Reader(Http2SocketAdapter socket, TimerGenerator& generator,
                   Http2StreamManager& stream_manager, Http2Settings params);
        
        /// 读取一个完整的 HTTP/2 帧
        AsyncResult<std::expected<Http2Frame::ptr, Http2Error>>
            readFrame(std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));
        
        /// 读取连接前言（服务器端）
        AsyncResult<std::expected<void, Http2Error>>
            readPreface(std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));
        
    private:
        Coroutine<nil> readFrameInternal(
            std::shared_ptr<AsyncWaiter<Http2Frame::ptr, Http2Error>> waiter,
            std::chrono::milliseconds timeout);
        
        Coroutine<nil> readPrefaceInternal(
            std::shared_ptr<AsyncWaiter<void, Http2Error>> waiter,
            std::chrono::milliseconds timeout);
        
    private:
        Http2SocketAdapter m_socket;
        Http2Settings m_params;
        TimerGenerator& m_generator;
        Http2StreamManager& m_stream_manager;
        Buffer m_buffer;
    };
}

#endif // GALAY_HTTP2_READER_H

