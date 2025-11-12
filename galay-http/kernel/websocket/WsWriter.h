#ifndef GALAY_WS_WRITER_H
#define GALAY_WS_WRITER_H

#include <galay/kernel/async/Socket.h>
#include <galay/kernel/async/TimerGenerator.h>
#include <galay/kernel/coroutine/AsyncWaiter.hpp>
#include "galay-http/protoc/websocket/WsFrame.h"
#include "galay-http/protoc/websocket/WsError.h"
#include "WsParams.hpp"
#include "galay/kernel/coroutine/CoSchedulerHandle.hpp"

namespace galay::http
{
    class WsWriter
    {
    public:
        WsWriter(AsyncTcpSocket& socket, CoSchedulerHandle handle, const WsSettings& params);

        // 发送一个 WebSocket 帧
        AsyncResult<std::expected<void, WsError>> 
            sendFrame(WsFrame& frame, 
                     std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));

        // 发送文本消息
        AsyncResult<std::expected<void, WsError>> 
            sendText(const std::string& text, 
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));

        // 发送二进制消息
        AsyncResult<std::expected<void, WsError>> 
            sendBinary(const std::string& data, 
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));

        // 发送 Ping
        AsyncResult<std::expected<void, WsError>> 
            sendPing(const std::string& payload = "", 
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));

        // 发送 Pong
        AsyncResult<std::expected<void, WsError>> 
            sendPong(const std::string& payload = "", 
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));

        // 发送关闭帧
        AsyncResult<std::expected<void, WsError>> 
            sendClose(WsCloseCode code = WsCloseCode::Normal, 
                     const std::string& reason = "",
                     std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));

        // 发送分片消息（用于大消息）
        AsyncResult<std::expected<void, WsError>> 
            sendFragmentedText(const std::string& text, 
                             size_t fragment_size,
                             std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));

        AsyncResult<std::expected<void, WsError>> 
            sendFragmentedBinary(const std::string& data, 
                               size_t fragment_size,
                               std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));

    private:
        Coroutine<nil> sendFrameInternal(
            std::string data,
            std::shared_ptr<AsyncWaiter<void, WsError>> waiter,
            std::chrono::milliseconds timeout);

        Coroutine<nil> sendData(
            const std::string& data,
            std::shared_ptr<AsyncWaiter<void, WsError>> waiter,
            std::chrono::milliseconds timeout);

    private:
        WsSettings          m_params;
        AsyncTcpSocket&     m_socket;
        CoSchedulerHandle   m_handle;
    };
}

#endif

