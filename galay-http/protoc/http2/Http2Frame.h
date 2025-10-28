#ifndef GALAY_HTTP2_FRAME_H
#define GALAY_HTTP2_FRAME_H

#include "Http2Base.h"
#include "Http2Error.h"
#include "Http2Hpack.h"
#include <memory>
#include <vector>
#include <expected>
#include <map>

namespace galay::http
{
    /**
     * @brief HTTP/2 帧头部
     * 
     * 所有 HTTP/2 帧都有一个固定的 9 字节头部：
     * +-----------------------------------------------+
     * |                 Length (24)                   |
     * +---------------+---------------+---------------+
     * |   Type (8)    |   Flags (8)   |
     * +-+-------------+---------------+-------------------------------+
     * |R|                 Stream Identifier (31)                      |
     * +=+=============================================================+
     */
    struct Http2FrameHeader
    {
        uint32_t length;            // 帧负载长度（24位，不包括9字节头部）
        Http2FrameType type;        // 帧类型
        uint8_t flags;              // 标志位
        uint32_t stream_id;         // 流标识符（31位，最高位保留）
        
        Http2FrameHeader()
            : length(0), type(Http2FrameType::HTTP2_UNKNOWN), flags(0), stream_id(0) {}
        
        Http2FrameHeader(uint32_t len, Http2FrameType t, uint8_t f, uint32_t sid)
            : length(len), type(t), flags(f), stream_id(sid & 0x7FFFFFFF) {}
        
        // 序列化为9字节
        std::string serialize() const;
        
        // 从9字节反序列化
        static std::expected<Http2FrameHeader, Http2Error> deserialize(const uint8_t* data, size_t length);
    };
    
    /**
     * @brief HTTP/2 帧基类
     */
    class Http2Frame
    {
    public:
        using ptr = std::shared_ptr<Http2Frame>;
        using uptr = std::unique_ptr<Http2Frame>;
        
        Http2Frame() = default;
        Http2Frame(const Http2FrameHeader& header) : m_header(header) {}
        virtual ~Http2Frame() = default;
        
        // Getters
        const Http2FrameHeader& header() const { return m_header; }
        Http2FrameHeader& header() { return m_header; }
        
        uint32_t length() const { return m_header.length; }
        Http2FrameType type() const { return m_header.type; }
        uint8_t flags() const { return m_header.flags; }
        uint32_t streamId() const { return m_header.stream_id; }
        
        // 序列化整个帧（头部 + 负载）
        virtual std::string serialize() const = 0;
        
        // 从数据反序列化（仅负载部分，头部已经解析）
        virtual std::expected<void, Http2Error> deserializePayload(const uint8_t* data, size_t length) = 0;
        
        // 工厂方法：根据帧头创建对应类型的帧
        static std::expected<Http2Frame::ptr, Http2Error> createFrame(const Http2FrameHeader& header);
        
    protected:
        Http2FrameHeader m_header;
    };
    
    /**
     * @brief DATA 帧 - 传输应用数据
     */
    class Http2DataFrame : public Http2Frame
    {
    public:
        Http2DataFrame();
        Http2DataFrame(uint32_t stream_id, const std::string& data, bool end_stream = false, uint8_t padding = 0);
        
        const std::string& data() const { return m_data; }
        std::string& data() { return m_data; }
        void setData(const std::string& data) { m_data = data; }
        
        uint8_t paddingLength() const { return m_padding_length; }
        bool endStream() const { return m_header.flags & FLAG_END_STREAM; }
        
        std::string serialize() const override;
        std::expected<void, Http2Error> deserializePayload(const uint8_t* data, size_t length) override;
        
    private:
        std::string m_data;
        uint8_t m_padding_length;
    };
    
    /**
     * @brief HEADERS 帧 - 传输头部字段
     */
    class Http2HeadersFrame : public Http2Frame
    {
    public:
        Http2HeadersFrame();
        Http2HeadersFrame(uint32_t stream_id, const std::string& header_block, 
                         bool end_stream = false, bool end_headers = true);
        
        const std::string& headerBlock() const { return m_header_block; }
        void setHeaderBlock(const std::string& block) { m_header_block = block; }
        
        // HPACK 辅助方法
        /**
         * @brief 从头部列表创建 HEADERS 帧（使用 HPACK 编码）
         * @param stream_id 流 ID
         * @param headers 头部列表
         * @param encoder HPACK 编码器
         * @param end_stream 是否结束流
         * @param end_headers 是否结束头部
         */
        static Http2HeadersFrame fromHeaders(uint32_t stream_id,
                                             const std::vector<HpackHeaderField>& headers,
                                             HpackEncoder& encoder,
                                             bool end_stream = false,
                                             bool end_headers = true);
        
        /**
         * @brief 解码头部块到头部列表（使用 HPACK 解码）
         * @param decoder HPACK 解码器
         * @return 头部列表，失败返回错误
         */
        std::expected<std::vector<HpackHeaderField>, Http2Error> 
        decodeHeaders(HpackDecoder& decoder) const;
        
        bool endStream() const { return m_header.flags & FLAG_END_STREAM; }
        bool endHeaders() const { return m_header.flags & FLAG_END_HEADERS; }
        bool hasPriority() const { return m_header.flags & FLAG_PRIORITY; }
        
        std::string serialize() const override;
        std::expected<void, Http2Error> deserializePayload(const uint8_t* data, size_t length) override;
        
    private:
        std::string m_header_block;  // HPACK 压缩后的头部
        uint8_t m_padding_length;
        
        // 优先级信息（如果 FLAG_PRIORITY 设置）
        bool m_exclusive;
        uint32_t m_stream_dependency;
        uint8_t m_weight;
    };
    
    /**
     * @brief PRIORITY 帧 - 指定流优先级
     */
    class Http2PriorityFrame : public Http2Frame
    {
    public:
        Http2PriorityFrame();
        Http2PriorityFrame(uint32_t stream_id, uint32_t dependency, uint8_t weight, bool exclusive = false);
        
        uint32_t streamDependency() const { return m_stream_dependency; }
        uint8_t weight() const { return m_weight; }
        bool exclusive() const { return m_exclusive; }
        
        std::string serialize() const override;
        std::expected<void, Http2Error> deserializePayload(const uint8_t* data, size_t length) override;
        
    private:
        bool m_exclusive;
        uint32_t m_stream_dependency;
        uint8_t m_weight;
    };
    
    /**
     * @brief RST_STREAM 帧 - 重置流
     */
    class Http2RstStreamFrame : public Http2Frame
    {
    public:
        Http2RstStreamFrame();
        Http2RstStreamFrame(uint32_t stream_id, Http2ErrorCode error_code);
        
        Http2ErrorCode errorCode() const { return m_error_code; }
        
        std::string serialize() const override;
        std::expected<void, Http2Error> deserializePayload(const uint8_t* data, size_t length) override;
        
    private:
        Http2ErrorCode m_error_code;
    };
    
    /**
     * @brief SETTINGS 帧 - 连接配置
     */
    class Http2SettingsFrame : public Http2Frame
    {
    public:
        Http2SettingsFrame();
        Http2SettingsFrame(bool ack);
        
        bool isAck() const { return m_header.flags & FLAG_ACK; }
        
        // 添加/获取设置项
        void setSetting(Http2SettingsId id, uint32_t value);
        std::expected<uint32_t, Http2Error> getSetting(Http2SettingsId id) const;
        const std::map<Http2SettingsId, uint32_t>& settings() const { return m_settings; }
        
        std::string serialize() const override;
        std::expected<void, Http2Error> deserializePayload(const uint8_t* data, size_t length) override;
        
    private:
        std::map<Http2SettingsId, uint32_t> m_settings;
    };
    
    /**
     * @brief PING 帧 - 连接保活和延迟测量
     */
    class Http2PingFrame : public Http2Frame
    {
    public:
        Http2PingFrame();
        Http2PingFrame(const uint8_t* opaque_data, bool ack = false);
        Http2PingFrame(uint64_t data, bool ack = false);
        
        bool isAck() const { return m_header.flags & FLAG_ACK; }
        const uint8_t* opaqueData() const { return m_opaque_data; }
        uint64_t data() const;
        
        std::string serialize() const override;
        std::expected<void, Http2Error> deserializePayload(const uint8_t* data, size_t length) override;
        
    private:
        uint8_t m_opaque_data[8];  // 8字节不透明数据
    };
    
    /**
     * @brief GOAWAY 帧 - 优雅关闭连接
     */
    class Http2GoAwayFrame : public Http2Frame
    {
    public:
        Http2GoAwayFrame();
        Http2GoAwayFrame(uint32_t last_stream_id, Http2ErrorCode error_code, const std::string& debug_data = "");
        
        uint32_t lastStreamId() const { return m_last_stream_id; }
        Http2ErrorCode errorCode() const { return m_error_code; }
        const std::string& debugData() const { return m_debug_data; }
        
        std::string serialize() const override;
        std::expected<void, Http2Error> deserializePayload(const uint8_t* data, size_t length) override;
        
    private:
        uint32_t m_last_stream_id;
        Http2ErrorCode m_error_code;
        std::string m_debug_data;
    };
    
    /**
     * @brief WINDOW_UPDATE 帧 - 流控窗口更新
     */
    class Http2WindowUpdateFrame : public Http2Frame
    {
    public:
        Http2WindowUpdateFrame();
        Http2WindowUpdateFrame(uint32_t stream_id, uint32_t window_size_increment);
        
        uint32_t windowSizeIncrement() const { return m_window_size_increment; }
        
        std::string serialize() const override;
        std::expected<void, Http2Error> deserializePayload(const uint8_t* data, size_t length) override;
        
    private:
        uint32_t m_window_size_increment;
    };
    
    /**
     * @brief CONTINUATION 帧 - 延续头部块
     */
    class Http2ContinuationFrame : public Http2Frame
    {
    public:
        Http2ContinuationFrame();
        Http2ContinuationFrame(uint32_t stream_id, const std::string& header_block, bool end_headers = true);
        
        const std::string& headerBlock() const { return m_header_block; }
        bool endHeaders() const { return m_header.flags & FLAG_END_HEADERS; }
        
        std::string serialize() const override;
        std::expected<void, Http2Error> deserializePayload(const uint8_t* data, size_t length) override;
        
    private:
        std::string m_header_block;
    };
}

#endif // GALAY_HTTP2_FRAME_H

