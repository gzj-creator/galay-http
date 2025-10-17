#ifndef GALAY_WS_FRAME_H
#define GALAY_WS_FRAME_H

#include "WsBase.h"
#include "WsError.h"
#include <memory>
#include <cstring>
#include <expected>

namespace galay::http
{
    // WebSocket 帧结构
    class WsFrame
    {
    public:
        using ptr = std::shared_ptr<WsFrame>;
        using uptr = std::unique_ptr<WsFrame>;

        WsFrame();
        WsFrame(WsOpcode opcode, const std::string& payload, bool fin = true);
        WsFrame(WsOpcode opcode, std::string&& payload, bool fin = true);

        // Getters
        bool fin() const { return m_fin; }
        bool rsv1() const { return m_rsv1; }
        bool rsv2() const { return m_rsv2; }
        bool rsv3() const { return m_rsv3; }
        WsOpcode opcode() const { return m_opcode; }
        bool mask() const { return m_mask; }
        uint64_t payloadLength() const { return m_payload_length; }
        const uint8_t* maskingKey() const { return m_masking_key; }
        const std::string& payload() const { return m_payload; }
        std::string& payload() { return m_payload; }

        // Setters
        void setFin(bool fin) { m_fin = fin; }
        void setRsv1(bool rsv1) { m_rsv1 = rsv1; }
        void setRsv2(bool rsv2) { m_rsv2 = rsv2; }
        void setRsv3(bool rsv3) { m_rsv3 = rsv3; }
        void setOpcode(WsOpcode opcode) { m_opcode = opcode; }
        void setMask(bool mask) { m_mask = mask; }
        void setMaskingKey(const uint8_t* key);
        void setPayload(const std::string& payload);
        void setPayload(std::string&& payload);

        // 序列化为字节流（发送时使用）
        std::string serialize() const;

        // 从字节流解析（接收时使用）
        // 返回解析成功的字节数，失败返回 0
        static std::expected<WsFrame, WsError> deserialize(const std::string& data);
        static std::expected<WsFrame, WsError> deserialize(const uint8_t* data, size_t length);

        // 工具方法
        static WsFrame createTextFrame(const std::string& text, bool mask = false);
        static WsFrame createBinaryFrame(const std::string& data, bool mask = false);
        static WsFrame createCloseFrame(WsCloseCode code = WsCloseCode::Normal, 
                                       const std::string& reason = "", bool mask = false);
        static WsFrame createPingFrame(const std::string& payload = "", bool mask = false);
        static WsFrame createPongFrame(const std::string& payload = "", bool mask = false);

        // 掩码操作
        void applyMask();
        static void applyMask(uint8_t* data, size_t length, const uint8_t* mask_key);

    private:
        bool m_fin;                         // FIN 位
        bool m_rsv1;                        // RSV1 位（保留）
        bool m_rsv2;                        // RSV2 位（保留）
        bool m_rsv3;                        // RSV3 位（保留）
        WsOpcode m_opcode;                  // 操作码
        bool m_mask;                        // MASK 位
        uint64_t m_payload_length;          // 载荷长度
        uint8_t m_masking_key[4];           // 掩码密钥
        std::string m_payload;              // 载荷数据
    };
}

#endif

