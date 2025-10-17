#include "WsFrame.h"
#include <random>
#include <cstring>

namespace galay::http
{
    WsFrame::WsFrame()
        : m_fin(true), m_rsv1(false), m_rsv2(false), m_rsv3(false),
          m_opcode(WsOpcode::Text), m_mask(false), m_payload_length(0)
    {
        std::memset(m_masking_key, 0, 4);
    }

    WsFrame::WsFrame(WsOpcode opcode, const std::string& payload, bool fin)
        : m_fin(fin), m_rsv1(false), m_rsv2(false), m_rsv3(false),
          m_opcode(opcode), m_mask(false), m_payload_length(payload.size()),
          m_payload(payload)
    {
        std::memset(m_masking_key, 0, 4);
    }

    WsFrame::WsFrame(WsOpcode opcode, std::string&& payload, bool fin)
        : m_fin(fin), m_rsv1(false), m_rsv2(false), m_rsv3(false),
          m_opcode(opcode), m_mask(false), m_payload_length(payload.size()),
          m_payload(std::move(payload))
    {
        std::memset(m_masking_key, 0, 4);
    }

    void WsFrame::setMaskingKey(const uint8_t* key)
    {
        if (key) {
            std::memcpy(m_masking_key, key, 4);
            m_mask = true;
        }
    }

    void WsFrame::setPayload(const std::string& payload)
    {
        m_payload = payload;
        m_payload_length = payload.size();
    }

    void WsFrame::setPayload(std::string&& payload)
    {
        m_payload_length = payload.size();
        m_payload = std::move(payload);
    }

    std::string WsFrame::serialize() const
    {
        std::string frame;
        
        // 第一个字节：FIN, RSV1-3, Opcode
        uint8_t byte1 = static_cast<uint8_t>(m_opcode);
        if (m_fin) byte1 |= 0x80;
        if (m_rsv1) byte1 |= 0x40;
        if (m_rsv2) byte1 |= 0x20;
        if (m_rsv3) byte1 |= 0x10;
        frame.push_back(byte1);

        // 第二个字节及后续：MASK, Payload length
        uint8_t byte2 = 0;
        if (m_mask) byte2 |= 0x80;

        if (m_payload_length < 126) {
            byte2 |= static_cast<uint8_t>(m_payload_length);
            frame.push_back(byte2);
        } else if (m_payload_length < 65536) {
            byte2 |= 126;
            frame.push_back(byte2);
            frame.push_back(static_cast<uint8_t>((m_payload_length >> 8) & 0xFF));
            frame.push_back(static_cast<uint8_t>(m_payload_length & 0xFF));
        } else {
            byte2 |= 127;
            frame.push_back(byte2);
            for (int i = 7; i >= 0; --i) {
                frame.push_back(static_cast<uint8_t>((m_payload_length >> (i * 8)) & 0xFF));
            }
        }

        // Masking key（如果有）
        if (m_mask) {
            frame.append(reinterpret_cast<const char*>(m_masking_key), 4);
        }

        // Payload data
        if (m_mask && !m_payload.empty()) {
            std::string masked_payload = m_payload;
            applyMask(reinterpret_cast<uint8_t*>(masked_payload.data()), 
                     masked_payload.size(), m_masking_key);
            frame.append(masked_payload);
        } else {
            frame.append(m_payload);
        }

        return frame;
    }

    std::expected<WsFrame, WsError> WsFrame::deserialize(const std::string& data)
    {
        return deserialize(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    }

    std::expected<WsFrame, WsError> WsFrame::deserialize(const uint8_t* data, size_t length)
    {
        if (length < 2) {
            return std::unexpected(WsError(kWsError_InvalidFrame));
        }

        WsFrame frame;
        size_t offset = 0;

        // 解析第一个字节
        uint8_t byte1 = data[offset++];
        frame.m_fin = (byte1 & 0x80) != 0;
        frame.m_rsv1 = (byte1 & 0x40) != 0;
        frame.m_rsv2 = (byte1 & 0x20) != 0;
        frame.m_rsv3 = (byte1 & 0x10) != 0;
        frame.m_opcode = static_cast<WsOpcode>(byte1 & 0x0F);

        // 检查保留位（如果没有使用扩展，保留位必须为 0）
        if (frame.m_rsv1 || frame.m_rsv2 || frame.m_rsv3) {
            return std::unexpected(WsError(kWsError_ReservedBitSet));
        }

        // 解析第二个字节
        uint8_t byte2 = data[offset++];
        frame.m_mask = (byte2 & 0x80) != 0;
        uint8_t payload_len = byte2 & 0x7F;

        // 解析载荷长度
        if (payload_len < 126) {
            frame.m_payload_length = payload_len;
        } else if (payload_len == 126) {
            if (length < offset + 2) {
                return std::unexpected(WsError(kWsError_InvalidFrame));
            }
            frame.m_payload_length = (static_cast<uint64_t>(data[offset]) << 8) | 
                                    static_cast<uint64_t>(data[offset + 1]);
            offset += 2;
        } else {  // payload_len == 127
            if (length < offset + 8) {
                return std::unexpected(WsError(kWsError_InvalidFrame));
            }
            frame.m_payload_length = 0;
            for (int i = 0; i < 8; ++i) {
                frame.m_payload_length = (frame.m_payload_length << 8) | 
                                        static_cast<uint64_t>(data[offset + i]);
            }
            offset += 8;
        }

        // 解析掩码密钥
        if (frame.m_mask) {
            if (length < offset + 4) {
                return std::unexpected(WsError(kWsError_InvalidFrame));
            }
            std::memcpy(frame.m_masking_key, data + offset, 4);
            offset += 4;
        }

        // 解析载荷数据
        if (length < offset + frame.m_payload_length) {
            return std::unexpected(WsError(kWsError_InvalidFrame));
        }

        frame.m_payload.assign(reinterpret_cast<const char*>(data + offset), 
                              frame.m_payload_length);

        // 如果有掩码，需要解码
        if (frame.m_mask) {
            applyMask(reinterpret_cast<uint8_t*>(frame.m_payload.data()), 
                     frame.m_payload.size(), frame.m_masking_key);
        }

        return frame;
    }

    WsFrame WsFrame::createTextFrame(const std::string& text, bool mask)
    {
        WsFrame frame(WsOpcode::Text, text, true);
        if (mask) {
            // 生成随机掩码
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(0, 255);
            uint8_t masking_key[4];
            for (int i = 0; i < 4; ++i) {
                masking_key[i] = static_cast<uint8_t>(dis(gen));
            }
            frame.setMaskingKey(masking_key);
        }
        return frame;
    }

    WsFrame WsFrame::createBinaryFrame(const std::string& data, bool mask)
    {
        WsFrame frame(WsOpcode::Binary, data, true);
        if (mask) {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(0, 255);
            uint8_t masking_key[4];
            for (int i = 0; i < 4; ++i) {
                masking_key[i] = static_cast<uint8_t>(dis(gen));
            }
            frame.setMaskingKey(masking_key);
        }
        return frame;
    }

    WsFrame WsFrame::createCloseFrame(WsCloseCode code, const std::string& reason, bool mask)
    {
        std::string payload;
        // 关闭帧的载荷：前 2 字节是状态码（大端序），后续是原因（可选）
        uint16_t code_value = static_cast<uint16_t>(code);
        payload.push_back(static_cast<char>((code_value >> 8) & 0xFF));
        payload.push_back(static_cast<char>(code_value & 0xFF));
        payload.append(reason);

        WsFrame frame(WsOpcode::Close, std::move(payload), true);
        if (mask) {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(0, 255);
            uint8_t masking_key[4];
            for (int i = 0; i < 4; ++i) {
                masking_key[i] = static_cast<uint8_t>(dis(gen));
            }
            frame.setMaskingKey(masking_key);
        }
        return frame;
    }

    WsFrame WsFrame::createPingFrame(const std::string& payload, bool mask)
    {
        WsFrame frame(WsOpcode::Ping, payload, true);
        if (mask) {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(0, 255);
            uint8_t masking_key[4];
            for (int i = 0; i < 4; ++i) {
                masking_key[i] = static_cast<uint8_t>(dis(gen));
            }
            frame.setMaskingKey(masking_key);
        }
        return frame;
    }

    WsFrame WsFrame::createPongFrame(const std::string& payload, bool mask)
    {
        WsFrame frame(WsOpcode::Pong, payload, true);
        if (mask) {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(0, 255);
            uint8_t masking_key[4];
            for (int i = 0; i < 4; ++i) {
                masking_key[i] = static_cast<uint8_t>(dis(gen));
            }
            frame.setMaskingKey(masking_key);
        }
        return frame;
    }

    void WsFrame::applyMask()
    {
        if (m_mask) {
            applyMask(reinterpret_cast<uint8_t*>(m_payload.data()), 
                     m_payload.size(), m_masking_key);
        }
    }

    void WsFrame::applyMask(uint8_t* data, size_t length, const uint8_t* mask_key)
    {
        for (size_t i = 0; i < length; ++i) {
            data[i] ^= mask_key[i % 4];
        }
    }
}

