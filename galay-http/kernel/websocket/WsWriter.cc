#include "WsWriter.h"
#include "galay-http/utils/WsDebugLog.h"

namespace galay::http
{
    WsWriter::WsWriter(AsyncTcpSocket& socket, TimerGenerator& generator, const WsSettings& params)
        : m_socket(socket), m_generator(generator), m_params(params)
    {
    }

    AsyncResult<std::expected<void, WsError>> 
    WsWriter::sendFrame(WsFrame& frame, std::chrono::milliseconds timeout)
    {
        WS_LOG_DEBUG("[WsWriter] Sending frame");
        if (timeout.count() == -1) {
            timeout = m_params.send_timeout;
        }
        
        auto waiter = std::make_shared<AsyncWaiter<void, WsError>>();
        waiter->appendTask(sendFrameInternal(frame.serialize(), waiter, timeout));
        return waiter->wait();
    }

    AsyncResult<std::expected<void, WsError>> 
    WsWriter::sendText(const std::string& text, std::chrono::milliseconds timeout)
    {
        WS_LOG_DEBUG("[WsWriter] Send text, size: {}", text.size());
        if (timeout.count() == -1) {
            timeout = m_params.send_timeout;
        }
        
        // 服务端发送的帧不需要掩码
        WsFrame frame = WsFrame::createTextFrame(text, false);
        return sendFrame(frame, timeout);
    }

    AsyncResult<std::expected<void, WsError>> 
    WsWriter::sendBinary(const std::string& data, std::chrono::milliseconds timeout)
    {
        WS_LOG_DEBUG("[WsWriter] Send binary, size: {}", data.size());
        if (timeout.count() == -1) {
            timeout = m_params.send_timeout;
        }
        
        WsFrame frame = WsFrame::createBinaryFrame(data, false);
        return sendFrame(frame, timeout);
    }

    AsyncResult<std::expected<void, WsError>> 
    WsWriter::sendPing(const std::string& payload, std::chrono::milliseconds timeout)
    {
        if (timeout.count() == -1) {
            timeout = m_params.send_timeout;
        }
        
        WsFrame frame = WsFrame::createPingFrame(payload, false);
        return sendFrame(frame, timeout);
    }

    AsyncResult<std::expected<void, WsError>> 
    WsWriter::sendPong(const std::string& payload, std::chrono::milliseconds timeout)
    {
        if (timeout.count() == -1) {
            timeout = m_params.send_timeout;
        }
        
        WsFrame frame = WsFrame::createPongFrame(payload, false);
        return sendFrame(frame, timeout);
    }

    AsyncResult<std::expected<void, WsError>> 
    WsWriter::sendClose(WsCloseCode code, const std::string& reason, 
                       std::chrono::milliseconds timeout)
    {
        WS_LOG_DEBUG("[WsWriter] Send close, code: {}", static_cast<int>(code));
        if (timeout.count() == -1) {
            timeout = m_params.send_timeout;
        }
        
        WsFrame frame = WsFrame::createCloseFrame(code, reason, false);
        return sendFrame(frame, timeout);
    }

    AsyncResult<std::expected<void, WsError>> 
    WsWriter::sendFragmentedText(const std::string& text, size_t fragment_size,
                                std::chrono::milliseconds timeout)
    {
        if (timeout.count() == -1) {
            timeout = m_params.send_timeout;
        }
        
        // 创建一个合并的帧并发送
        auto waiter = std::make_shared<AsyncWaiter<void, WsError>>();
        WsFrame frame = WsFrame::createTextFrame(text, false);
        waiter->appendTask(sendFrameInternal(frame.serialize(), waiter, timeout));
        return waiter->wait();
    }

    AsyncResult<std::expected<void, WsError>> 
    WsWriter::sendFragmentedBinary(const std::string& data, size_t fragment_size,
                                  std::chrono::milliseconds timeout)
    {
        if (timeout.count() == -1) {
            timeout = m_params.send_timeout;
        }
        
        // 创建一个合并的帧并发送
        auto waiter = std::make_shared<AsyncWaiter<void, WsError>>();
        WsFrame frame = WsFrame::createBinaryFrame(data, false);
        waiter->appendTask(sendFrameInternal(frame.serialize(), waiter, timeout));
        return waiter->wait();
    }

    Coroutine<nil> WsWriter::sendFrameInternal(
        std::string data,
        std::shared_ptr<AsyncWaiter<void, WsError>> waiter,
        std::chrono::milliseconds timeout)
    {
        // 序列化帧
        
        // 发送数据
        auto bytes = Bytes::fromString(data);
        
        while (true) {
            std::expected<Bytes, CommonError> res;
            if (timeout < std::chrono::milliseconds(0)) {
                res = co_await m_socket.send(std::move(bytes));
            } else {
                auto temp = co_await m_generator.timeout<std::expected<Bytes, CommonError>>([&](){
                    return m_socket.send(std::move(bytes));
                }, timeout);
                if (!temp) {
                    waiter->notify(std::unexpected(WsError(kWsError_SendTimeOut)));
                    co_return nil();
                }
                res = std::move(temp.value());
            }
            
            if (res) {
                bytes = std::move(res.value());
                if (bytes.empty()) {
                    break;
                }
            } else {
                if (CommonError::contains(res.error().code(), error::DisConnectError)) {
                    waiter->notify(std::unexpected(WsError(kWsError_ConnectionClose)));
                } else {
                    waiter->notify(std::unexpected(WsError(kWsError_TcpSendError)));
                }
                co_return nil();
            }
        }
        
        waiter->notify({});
        co_return nil();
    }

    Coroutine<nil> WsWriter::sendData(
        const std::string& data,
        std::shared_ptr<AsyncWaiter<void, WsError>> waiter,
        std::chrono::milliseconds timeout)
    {
        auto bytes = Bytes::fromString(data);
        
        while (true) {
            std::expected<Bytes, CommonError> res;
            if (timeout < std::chrono::milliseconds(0)) {
                res = co_await m_socket.send(std::move(bytes));
            } else {
                auto temp = co_await m_generator.timeout<std::expected<Bytes, CommonError>>([&](){
                    return m_socket.send(std::move(bytes));
                }, timeout);
                if (!temp) {
                    waiter->notify(std::unexpected(WsError(kWsError_SendTimeOut)));
                    co_return nil();
                }
                res = std::move(temp.value());
            }
            
            if (res) {
                bytes = std::move(res.value());
                if (bytes.empty()) {
                    break;
                }
            } else {
                if (CommonError::contains(res.error().code(), error::DisConnectError)) {
                    waiter->notify(std::unexpected(WsError(kWsError_ConnectionClose)));
                } else {
                    waiter->notify(std::unexpected(WsError(kWsError_TcpSendError)));
                }
                co_return nil();
            }
        }
        
        waiter->notify({});
        co_return nil();
    }
}

