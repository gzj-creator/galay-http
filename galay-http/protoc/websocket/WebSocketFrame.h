#ifndef GALAY_WEBSOCKET_FRAME_H
#define GALAY_WEBSOCKET_FRAME_H

#include "WebSocketBase.h"
#include "WebSocketError.h"
#include <expected>
#include <vector>
#include <sys/uio.h>

namespace galay::websocket
{

/**
 * @brief WebSocket 帧解析器
 * @details 提供WebSocket帧的解析和编码功能
 */
class WsFrameParser
{
public:
    /**
     * @brief 从iovec解析WebSocket帧
     * @param iovecs 输入的iovec数组
     * @param frame 输出的帧数据
     * @param is_server 是否是服务器端（服务器端要求客户端必须使用掩码）
     * @return std::expected<size_t, WsError>
     *         - size_t: 消费的字节数
     *         - WsError: 解析错误或数据不完整
     */
    static std::expected<size_t, WsError>
    fromIOVec(const std::vector<iovec>& iovecs, WsFrame& frame, bool is_server = true);

    /**
     * @brief 将WebSocket帧编码为字节流
     * @param frame 要编码的帧
     * @param use_mask 是否使用掩码（客户端必须使用）
     * @return 编码后的字节流
     */
    static std::string toBytes(const WsFrame& frame, bool use_mask = false);

    /**
     * @brief 创建文本帧
     * @param text 文本数据
     * @param fin 是否是最后一个分片
     * @return WsFrame
     */
    static WsFrame createTextFrame(const std::string& text, bool fin = true)
    {
        return WsFrame(WsOpcode::Text, text, fin);
    }

    /**
     * @brief 创建二进制帧
     * @param data 二进制数据
     * @param fin 是否是最后一个分片
     * @return WsFrame
     */
    static WsFrame createBinaryFrame(const std::string& data, bool fin = true)
    {
        return WsFrame(WsOpcode::Binary, data, fin);
    }

    /**
     * @brief 创建关闭帧
     * @param code 关闭状态码
     * @param reason 关闭原因
     * @return WsFrame
     */
    static WsFrame createCloseFrame(WsCloseCode code = WsCloseCode::Normal,
                                   const std::string& reason = "");

    /**
     * @brief 创建Ping帧
     * @param data Ping数据（可选）
     * @return WsFrame
     */
    static WsFrame createPingFrame(const std::string& data = "")
    {
        return WsFrame(WsOpcode::Ping, data, true);
    }

    /**
     * @brief 创建Pong帧
     * @param data Pong数据（通常是Ping的数据）
     * @return WsFrame
     */
    static WsFrame createPongFrame(const std::string& data = "")
    {
        return WsFrame(WsOpcode::Pong, data, true);
    }

    /**
     * @brief 应用掩码
     * @param data 要掩码的数据
     * @param masking_key 掩码密钥
     */
    static void applyMask(std::string& data, const uint8_t masking_key[4]);

    /**
     * @brief 验证UTF-8编码
     * @param data 要验证的数据
     * @return true表示有效的UTF-8
     */
    static bool isValidUtf8(const std::string& data);

private:
    /**
     * @brief 从iovec读取指定长度的数据
     * @param iovecs iovec数组
     * @param offset 起始偏移量
     * @param length 要读取的长度
     * @param output 输出buffer
     * @return 实际读取的字节数
     */
    static size_t readData(const std::vector<iovec>& iovecs,
                          size_t offset,
                          size_t length,
                          std::string& output);

    /**
     * @brief 计算iovec总长度
     */
    static size_t getTotalLength(const std::vector<iovec>& iovecs);

    /**
     * @brief 从iovec读取单个字节
     */
    static bool readByte(const std::vector<iovec>& iovecs, size_t offset, uint8_t& byte);

    /**
     * @brief 从iovec读取uint16_t（大端序）
     */
    static bool readUint16(const std::vector<iovec>& iovecs, size_t offset, uint16_t& value);

    /**
     * @brief 从iovec读取uint64_t（大端序）
     */
    static bool readUint64(const std::vector<iovec>& iovecs, size_t offset, uint64_t& value);
};

} // namespace galay::websocket

#endif // GALAY_WEBSOCKET_FRAME_H
