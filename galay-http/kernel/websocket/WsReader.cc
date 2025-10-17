#include "WsReader.h"
#include "galay-http/utils/HttpLogger.h"

namespace galay::http
{
    WsReader::WsReader(AsyncTcpSocket& socket, TimerGenerator& generator, WsSettings params)
        : m_socket(socket), m_generator(generator), m_params(params),
          m_in_fragment(false), m_fragment_opcode(WsOpcode::Unknown)
    {
    }

    AsyncResult<std::expected<WsFrame, WsError>> 
    WsReader::readFrame(std::chrono::milliseconds timeout)
    {
        if (timeout.count() == -1) {
            timeout = m_params.recv_timeout;
        }
        
        auto waiter = std::make_shared<AsyncWaiter<WsFrame, WsError>>();
        waiter->appendTask(readFrameInternal(waiter, timeout));
        return waiter->wait();
    }

    AsyncResult<std::expected<std::string, WsError>> 
    WsReader::readMessage(std::chrono::milliseconds timeout)
    {
        if (timeout.count() == -1) {
            timeout = m_params.recv_timeout;
        }
        
        auto waiter = std::make_shared<AsyncWaiter<std::string, WsError>>();
        waiter->appendTask(readMessageInternal(waiter, timeout));
        return waiter->wait();
    }

    AsyncResult<std::expected<std::string, WsError>> 
    WsReader::readTextMessage(std::chrono::milliseconds timeout)
    {
        return readMessage(timeout);
    }

    AsyncResult<std::expected<std::string, WsError>> 
    WsReader::readBinaryMessage(std::chrono::milliseconds timeout)
    {
        return readMessage(timeout);
    }

    Coroutine<nil> WsReader::readFrameInternal(
        std::shared_ptr<AsyncWaiter<WsFrame, WsError>> waiter,
        std::chrono::milliseconds timeout)
    {
        // 参考 HttpReader 的实现，确保缓冲区正确初始化
        if (m_buffer.capacity() == 0) {
            m_buffer = Buffer(m_params.max_frame_size);
        }
        if(m_buffer.capacity() < 2) {
            m_buffer = Buffer(m_params.max_frame_size);
        }
        
        size_t recv_size = 0;
        
        // 至少读取 2 字节的基本头部
        while (recv_size < 2) {
            
            std::expected<Bytes, CommonError> bytes;
            if (timeout < std::chrono::milliseconds(0)) {
                bytes = co_await m_socket.recv(m_buffer.data() + recv_size, m_buffer.capacity() - recv_size);
            } else {
                auto res = co_await m_generator.timeout<std::expected<Bytes, CommonError>>([&, this](){
                    return m_socket.recv(m_buffer.data() + recv_size, m_buffer.capacity() - recv_size);
                }, timeout);
                if (!res) {
                    HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[{}] [WsReader] Recv timeout", __LINE__);
                    waiter->notify(std::unexpected(WsError(kWsError_RecvTimeOut)));
                    co_return nil{};
                }
                bytes = std::move(res.value());
            }

            if (!bytes) {
                if (CommonError::contains(bytes.error().code(), error::DisConnectError)) {
                    waiter->notify(std::unexpected(WsError(kWsError_ConnectionClose)));
                } else {
                    waiter->notify(std::unexpected(WsError(kWsError_TcpRecvError)));
                }
                co_return nil{};
            }

            if (bytes.value().empty()) {
                waiter->notify(std::unexpected(WsError(kWsError_ConnectionClose)));
                co_return nil{};
            }
            recv_size += bytes.value().size();
        }

        // 解析基本头部
        uint8_t byte1 = static_cast<uint8_t>(m_buffer.data()[0]);
        uint8_t byte2 = static_cast<uint8_t>(m_buffer.data()[1]);
        
        bool fin = (byte1 & 0x80) != 0;
        bool rsv1 = (byte1 & 0x40) != 0;
        bool rsv2 = (byte1 & 0x20) != 0;
        bool rsv3 = (byte1 & 0x10) != 0;
        WsOpcode opcode = static_cast<WsOpcode>(byte1 & 0x0F);
        
        bool mask = (byte2 & 0x80) != 0;
        uint8_t payload_len = byte2 & 0x7F;

        // 检查保留位
        if (rsv1 || rsv2 || rsv3) {
            waiter->notify(std::unexpected(WsError(kWsError_ReservedBitSet)));
            co_return nil{};
        }

        // 检查控制帧不能分片
        if (!fin && isControlFrame(opcode)) {
            waiter->notify(std::unexpected(WsError(kWsError_FragmentedControl)));
            co_return nil{};
        }

        // 计算需要的总头部长度
        size_t header_size = 2;
        if (payload_len == 126) {
            header_size += 2;
        } else if (payload_len == 127) {
            header_size += 8;
        }
        if (mask) {
            header_size += 4;
        }

        // 继续读取直到获取完整头部
        while (recv_size < header_size) {
            std::expected<Bytes, CommonError> bytes;
            if (timeout < std::chrono::milliseconds(0)) {
                bytes = co_await m_socket.recv(m_buffer.data() + recv_size, m_buffer.capacity() - recv_size);
            } else {
                auto res = co_await m_generator.timeout<std::expected<Bytes, CommonError>>([&, this](){
                    return m_socket.recv(m_buffer.data() + recv_size, m_buffer.capacity() - recv_size);
                }, timeout);
                if (!res) {
                    waiter->notify(std::unexpected(WsError(kWsError_RecvTimeOut)));
                    co_return nil{};
                }
                bytes = std::move(res.value());
            }

            if (!bytes) {
                if (CommonError::contains(bytes.error().code(), error::DisConnectError)) {
                    waiter->notify(std::unexpected(WsError(kWsError_ConnectionClose)));
                } else {
                    waiter->notify(std::unexpected(WsError(kWsError_TcpRecvError)));
                }
                co_return nil{};
            }

            if (bytes.value().empty()) {
                waiter->notify(std::unexpected(WsError(kWsError_ConnectionClose)));
                co_return nil{};
            }

            recv_size += bytes.value().size();
        }

        // 解析载荷长度
        uint64_t payload_length = payload_len;
        size_t offset = 2;
        if (payload_len == 126) {
            payload_length = (static_cast<uint64_t>(static_cast<uint8_t>(m_buffer.data()[offset])) << 8) |
                           static_cast<uint64_t>(static_cast<uint8_t>(m_buffer.data()[offset + 1]));
            offset += 2;
        } else if (payload_len == 127) {
            payload_length = 0;
            for (int i = 0; i < 8; ++i) {
                payload_length = (payload_length << 8) | 
                               static_cast<uint64_t>(static_cast<uint8_t>(m_buffer.data()[offset + i]));
            }
            offset += 8;
        }

        // 检查帧大小限制
        if (payload_length > m_params.max_frame_size) {
            waiter->notify(std::unexpected(WsError(kWsError_FrameTooLarge)));
            co_return nil{};
        }

        // 解析掩码密钥
        uint8_t masking_key[4] = {0};
        if (mask) {
            std::memcpy(masking_key, m_buffer.data() + offset, 4);
            offset += 4;
        }

        // 读取载荷数据
        size_t total_frame_size = offset + payload_length;
        while (recv_size < total_frame_size) {
            std::expected<Bytes, CommonError> bytes;
            if (timeout < std::chrono::milliseconds(0)) {
                bytes = co_await m_socket.recv(m_buffer.data() + recv_size, 
                                              std::min(m_buffer.capacity() - recv_size, total_frame_size - recv_size));
            } else {
                auto res = co_await m_generator.timeout<std::expected<Bytes, CommonError>>([&, this](){
                    return m_socket.recv(m_buffer.data() + recv_size, 
                                       std::min(m_buffer.capacity() - recv_size, total_frame_size - recv_size));
                }, timeout);
                if (!res) {
                    waiter->notify(std::unexpected(WsError(kWsError_RecvTimeOut)));
                    co_return nil{};
                }
                bytes = std::move(res.value());
            }

            if (!bytes) {
                if (CommonError::contains(bytes.error().code(), error::DisConnectError)) {
                    waiter->notify(std::unexpected(WsError(kWsError_ConnectionClose)));
                } else {
                    waiter->notify(std::unexpected(WsError(kWsError_TcpRecvError)));
                }
                co_return nil{};
            }

            if (bytes.value().empty()) {
                waiter->notify(std::unexpected(WsError(kWsError_ConnectionClose)));
                co_return nil{};
            }

            recv_size += bytes.value().size();
        }

        // 提取载荷
        std::string payload(m_buffer.data() + offset, payload_length);
        
        // 更新缓冲区（移除已处理的数据）
        if (recv_size > total_frame_size) {
            m_buffer = Buffer(m_buffer.data() + total_frame_size, recv_size - total_frame_size);
        } else {
            m_buffer.clear();
        }

        // 解除掩码
        if (mask && payload_length > 0) {
            WsFrame::applyMask(reinterpret_cast<uint8_t*>(payload.data()), 
                              payload.size(), masking_key);
        }

        // 验证 UTF-8（对于文本帧）
        if (m_params.validate_utf8 && opcode == WsOpcode::Text) {
            if (!validateUtf8(payload)) {
                waiter->notify(std::unexpected(WsError(kWsError_InvalidUTF8)));
                co_return nil{};
            }
        }

        // 创建帧对象
        WsFrame frame(opcode, std::move(payload), fin);
        frame.setRsv1(rsv1);
        frame.setRsv2(rsv2);
        frame.setRsv3(rsv3);
        if (mask) {
            frame.setMaskingKey(masking_key);
        }
        
        waiter->notify(std::move(frame));
        co_return nil{};
    }

    Coroutine<nil> WsReader::readMessageInternal(
        std::shared_ptr<AsyncWaiter<std::string, WsError>> waiter,
        std::chrono::milliseconds timeout)
    {
        std::string message;
        WsOpcode message_opcode = WsOpcode::Unknown;
        bool first_frame = true;

        while (true) {
            auto frame_waiter = std::make_shared<AsyncWaiter<WsFrame, WsError>>();
            frame_waiter->appendTask(readFrameInternal(frame_waiter, timeout));
            auto frame_result = co_await frame_waiter->wait();
            
            if (!frame_result.has_value()) {
                waiter->notify(std::unexpected(frame_result.error()));
                co_return nil{};
            }

            WsFrame& frame = frame_result.value();

            // 处理控制帧（可能出现在数据帧之间）
            if (isControlFrame(frame.opcode())) {
                // 控制帧需要立即处理，但不影响消息的组装
                // 这里只返回控制帧，由上层处理
                if (first_frame) {
                    waiter->notify(frame.payload());
                    co_return nil{};
                }
                // 如果在消息中间收到控制帧，暂时忽略（实际应该由上层处理）
                continue;
            }

            if (first_frame) {
                if (frame.opcode() == WsOpcode::Continuation) {
                    waiter->notify(std::unexpected(WsError(kWsError_UnexpectedContinuation)));
                    co_return nil{};
                }
                message_opcode = frame.opcode();
                first_frame = false;
            } else {
                if (frame.opcode() != WsOpcode::Continuation) {
                    waiter->notify(std::unexpected(WsError(kWsError_ProtocolError)));
                    co_return nil{};
                }
            }

            message.append(frame.payload());

            // 检查消息大小限制
            if (message.size() > m_params.max_message_size) {
                waiter->notify(std::unexpected(WsError(kWsError_MessageTooLarge)));
                co_return nil{};
            }

            if (frame.fin()) {
                break;
            }
        }

        waiter->notify(std::move(message));
        co_return nil{};
    }

    bool WsReader::validateUtf8(const std::string& str)
    {
        // 简单的 UTF-8 验证（可以使用更完善的库）
        size_t i = 0;
        while (i < str.size()) {
            unsigned char c = str[i];
            int num_bytes = 0;

            if ((c & 0x80) == 0) {
                num_bytes = 1;
            } else if ((c & 0xE0) == 0xC0) {
                num_bytes = 2;
            } else if ((c & 0xF0) == 0xE0) {
                num_bytes = 3;
            } else if ((c & 0xF8) == 0xF0) {
                num_bytes = 4;
            } else {
                return false;
            }

            if (i + num_bytes > str.size()) {
                return false;
            }

            for (int j = 1; j < num_bytes; ++j) {
                if ((str[i + j] & 0xC0) != 0x80) {
                    return false;
                }
            }

            i += num_bytes;
        }
        return true;
    }
}
