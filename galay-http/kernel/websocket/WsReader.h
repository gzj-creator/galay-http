#ifndef GALAY_WS_READER_H
#define GALAY_WS_READER_H

#include <galay/kernel/async/Socket.h>
#include <galay/kernel/async/TimerGenerator.h>
#include <galay/kernel/coroutine/AsyncWaiter.hpp>
#include "galay-http/protoc/websocket/WsFrame.h"
#include "galay-http/protoc/websocket/WsError.h"
#include "WsParams.hpp"
#include "galay/kernel/coroutine/CoSchedulerHandle.hpp"

namespace galay::http
{
    class WsReader
    {
    public:
        WsReader(AsyncTcpSocket& socket, CoSchedulerHandle handle, WsSettings params);

        // 读取一个完整的 WebSocket 帧
        AsyncResult<std::expected<WsFrame, WsError>> 
            readFrame(std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));

        // 读取一个完整的 WebSocket 消息（可能由多个帧组成）
        AsyncResult<std::expected<std::string, WsError>> 
            readMessage(std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));

        // 读取文本消息
        AsyncResult<std::expected<std::string, WsError>> 
            readTextMessage(std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));

        // 读取二进制消息
        AsyncResult<std::expected<std::string, WsError>> 
            readBinaryMessage(std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));

    private:
        Coroutine<nil> readFrameInternal(
            std::shared_ptr<AsyncWaiter<WsFrame, WsError>> waiter,
            std::chrono::milliseconds timeout);

        Coroutine<nil> readMessageInternal(
            std::shared_ptr<AsyncWaiter<std::string, WsError>> waiter,
            std::chrono::milliseconds timeout);

        // 验证 UTF-8 编码
        bool validateUtf8(const std::string& str);

    private:
        Buffer              m_buffer;
        WsSettings          m_params;
        AsyncTcpSocket&     m_socket;
        CoSchedulerHandle   m_handle;
        
        // 用于处理分片消息
        bool                m_in_fragment;
        WsOpcode            m_fragment_opcode;
        std::string         m_fragment_buffer;
        
    };
}

#endif

