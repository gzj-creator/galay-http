#ifndef GALAY_WS_READER_H
#define GALAY_WS_READER_H

#include <galay/kernel/async/Socket.h>
#include <galay/kernel/async/TimerGenerator.h>
#include <galay/kernel/coroutine/AsyncWaiter.hpp>
#include "galay-http/protoc/websocket/WsFrame.h"
#include "galay-http/protoc/websocket/WsError.h"
#include "WsParams.hpp"

namespace galay::http
{
    class WsReader
    {
    public:
        WsReader(AsyncTcpSocket& socket, TimerGenerator& generator, WsSettings params);

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
        AsyncTcpSocket& m_socket;
        WsSettings m_params;
        TimerGenerator& m_generator;
        Buffer m_buffer;
        
        // 用于处理分片消息
        std::string m_fragment_buffer;
        WsOpcode m_fragment_opcode;
        bool m_in_fragment;
    };
}

#endif

