#include "Http2Frame.h"
#include <cstring>
#include <arpa/inet.h>

namespace galay::http
{
    // ==================== Http2FrameHeader ====================
    
    std::string Http2FrameHeader::serialize() const
    {
        std::string result(9, '\0');
        uint8_t* data = reinterpret_cast<uint8_t*>(result.data());
        
        // Length (24 bits, big-endian)
        data[0] = (length >> 16) & 0xFF;
        data[1] = (length >> 8) & 0xFF;
        data[2] = length & 0xFF;
        
        // Type (8 bits)
        data[3] = static_cast<uint8_t>(type);
        
        // Flags (8 bits)
        data[4] = flags;
        
        // Stream ID (31 bits, big-endian, 最高位保留为0)
        uint32_t sid = stream_id & 0x7FFFFFFF;
        data[5] = (sid >> 24) & 0xFF;
        data[6] = (sid >> 16) & 0xFF;
        data[7] = (sid >> 8) & 0xFF;
        data[8] = sid & 0xFF;
        
        return result;
    }
    
    std::expected<Http2FrameHeader, Http2Error> Http2FrameHeader::deserialize(const uint8_t* data, size_t len)
    {
        if (len < 9) {
            return std::unexpected(Http2Error(kHttp2Error_InvalidFrameSize, "Header too short"));
        }
        
        Http2FrameHeader header;
        
        // Length (24 bits)
        header.length = (static_cast<uint32_t>(data[0]) << 16) |
                       (static_cast<uint32_t>(data[1]) << 8) |
                        static_cast<uint32_t>(data[2]);
        
        // Type
        header.type = static_cast<Http2FrameType>(data[3]);
        
        // Flags
        header.flags = data[4];
        
        // Stream ID (31 bits, 忽略最高位)
        header.stream_id = ((static_cast<uint32_t>(data[5]) << 24) |
                           (static_cast<uint32_t>(data[6]) << 16) |
                           (static_cast<uint32_t>(data[7]) << 8) |
                            static_cast<uint32_t>(data[8])) & 0x7FFFFFFF;
        
        return header;
    }
    
    // ==================== Http2Frame ====================
    
    std::expected<Http2Frame::ptr, Http2Error> Http2Frame::createFrame(const Http2FrameHeader& header)
    {
        Http2Frame::ptr frame;
        
        switch (header.type) {
            case Http2FrameType::DATA:
                frame = std::make_shared<Http2DataFrame>();
                break;
            case Http2FrameType::HEADERS:
                frame = std::make_shared<Http2HeadersFrame>();
                break;
            case Http2FrameType::PRIORITY:
                frame = std::make_shared<Http2PriorityFrame>();
                break;
            case Http2FrameType::RST_STREAM:
                frame = std::make_shared<Http2RstStreamFrame>();
                break;
            case Http2FrameType::SETTINGS:
                frame = std::make_shared<Http2SettingsFrame>();
                break;
            case Http2FrameType::PING:
                frame = std::make_shared<Http2PingFrame>();
                break;
            case Http2FrameType::GOAWAY:
                frame = std::make_shared<Http2GoAwayFrame>();
                break;
            case Http2FrameType::WINDOW_UPDATE:
                frame = std::make_shared<Http2WindowUpdateFrame>();
                break;
            case Http2FrameType::CONTINUATION:
                frame = std::make_shared<Http2ContinuationFrame>();
                break;
            default:
                return std::unexpected(Http2Error(kHttp2Error_InvalidFrameType));
        }
        
        frame->m_header = header;
        return frame;
    }
    
    // ==================== Http2DataFrame ====================
    
    Http2DataFrame::Http2DataFrame()
        : m_padding_length(0)
    {
        m_header.type = Http2FrameType::DATA;
    }
    
    Http2DataFrame::Http2DataFrame(uint32_t stream_id, const std::string& data, bool end_stream, uint8_t padding)
        : m_data(data), m_padding_length(padding)
    {
        m_header.type = Http2FrameType::DATA;
        m_header.stream_id = stream_id;
        m_header.flags = 0;
        if (end_stream) {
            m_header.flags |= FLAG_END_STREAM;
        }
        if (padding > 0) {
            m_header.flags |= FLAG_PADDED;
        }
        m_header.length = data.size() + (padding > 0 ? padding + 1 : 0);
    }
    
    std::string Http2DataFrame::serialize() const
    {
        std::string result = m_header.serialize();
        
        if (m_header.flags & FLAG_PADDED) {
            result += static_cast<char>(m_padding_length);
        }
        
        result += m_data;
        
        if (m_padding_length > 0) {
            result.append(m_padding_length, '\0');
        }
        
        return result;
    }
    
    std::expected<void, Http2Error> Http2DataFrame::deserializePayload(const uint8_t* data, size_t length)
    {
        size_t offset = 0;
        
        if (m_header.flags & FLAG_PADDED) {
            if (length < 1) {
                return std::unexpected(Http2Error(kHttp2Error_ProtocolError, "PADDED flag set but no padding length"));
            }
            m_padding_length = data[0];
            offset = 1;
        }
        
        if (offset + m_padding_length > length) {
            return std::unexpected(Http2Error(kHttp2Error_ProtocolError, "Padding length exceeds frame length"));
        }
        
        size_t data_length = length - offset - m_padding_length;
        m_data = std::string(reinterpret_cast<const char*>(data + offset), data_length);
        
        return {};
    }
    
    // ==================== Http2HeadersFrame ====================
    
    Http2HeadersFrame::Http2HeadersFrame()
        : m_padding_length(0), m_exclusive(false), m_stream_dependency(0), m_weight(0)
    {
        m_header.type = Http2FrameType::HEADERS;
    }
    
    Http2HeadersFrame::Http2HeadersFrame(uint32_t stream_id, const std::string& header_block, 
                                         bool end_stream, bool end_headers)
        : m_header_block(header_block), m_padding_length(0), 
          m_exclusive(false), m_stream_dependency(0), m_weight(0)
    {
        m_header.type = Http2FrameType::HEADERS;
        m_header.stream_id = stream_id;
        m_header.flags = 0;
        if (end_stream) {
            m_header.flags |= FLAG_END_STREAM;
        }
        if (end_headers) {
            m_header.flags |= FLAG_END_HEADERS;
        }
        m_header.length = header_block.size();
    }
    
    Http2HeadersFrame Http2HeadersFrame::fromHeaders(uint32_t stream_id,
                                                     const std::vector<HpackHeaderField>& headers,
                                                     HpackEncoder& encoder,
                                                     bool end_stream,
                                                     bool end_headers)
    {
        std::string header_block = encoder.encodeHeaders(headers, true);
        return Http2HeadersFrame(stream_id, header_block, end_stream, end_headers);
    }
    
    std::expected<std::vector<HpackHeaderField>, Http2Error> 
    Http2HeadersFrame::decodeHeaders(HpackDecoder& decoder) const
    {
        return decoder.decodeHeaders(
            reinterpret_cast<const uint8_t*>(m_header_block.data()), 
            m_header_block.size()
        );
    }
    
    std::string Http2HeadersFrame::serialize() const
    {
        std::string result = m_header.serialize();
        
        if (m_header.flags & FLAG_PADDED) {
            result += static_cast<char>(m_padding_length);
        }
        
        if (m_header.flags & FLAG_PRIORITY) {
            uint32_t dep = m_stream_dependency & 0x7FFFFFFF;
            if (m_exclusive) {
                dep |= 0x80000000;
            }
            result += static_cast<char>((dep >> 24) & 0xFF);
            result += static_cast<char>((dep >> 16) & 0xFF);
            result += static_cast<char>((dep >> 8) & 0xFF);
            result += static_cast<char>(dep & 0xFF);
            result += static_cast<char>(m_weight);
        }
        
        result += m_header_block;
        
        if (m_padding_length > 0) {
            result.append(m_padding_length, '\0');
        }
        
        return result;
    }
    
    std::expected<void, Http2Error> Http2HeadersFrame::deserializePayload(const uint8_t* data, size_t length)
    {
        size_t offset = 0;
        
        if (m_header.flags & FLAG_PADDED) {
            if (length < 1) {
                return std::unexpected(Http2Error(kHttp2Error_ProtocolError));
            }
            m_padding_length = data[0];
            offset = 1;
        }
        
        if (m_header.flags & FLAG_PRIORITY) {
            if (length < offset + 5) {
                return std::unexpected(Http2Error(kHttp2Error_ProtocolError));
            }
            uint32_t dep = (static_cast<uint32_t>(data[offset]) << 24) |
                          (static_cast<uint32_t>(data[offset + 1]) << 16) |
                          (static_cast<uint32_t>(data[offset + 2]) << 8) |
                           static_cast<uint32_t>(data[offset + 3]);
            m_exclusive = (dep & 0x80000000) != 0;
            m_stream_dependency = dep & 0x7FFFFFFF;
            m_weight = data[offset + 4];
            offset += 5;
        }
        
        if (offset + m_padding_length > length) {
            return std::unexpected(Http2Error(kHttp2Error_ProtocolError));
        }
        
        size_t block_length = length - offset - m_padding_length;
        m_header_block = std::string(reinterpret_cast<const char*>(data + offset), block_length);
        
        return {};
    }
    
    // ==================== Http2PriorityFrame ====================
    
    Http2PriorityFrame::Http2PriorityFrame()
        : m_exclusive(false), m_stream_dependency(0), m_weight(0)
    {
        m_header.type = Http2FrameType::PRIORITY;
        m_header.length = 5;
    }
    
    Http2PriorityFrame::Http2PriorityFrame(uint32_t stream_id, uint32_t dependency, uint8_t weight, bool exclusive)
        : m_exclusive(exclusive), m_stream_dependency(dependency), m_weight(weight)
    {
        m_header.type = Http2FrameType::PRIORITY;
        m_header.stream_id = stream_id;
        m_header.length = 5;
    }
    
    std::string Http2PriorityFrame::serialize() const
    {
        std::string result = m_header.serialize();
        
        uint32_t dep = m_stream_dependency & 0x7FFFFFFF;
        if (m_exclusive) {
            dep |= 0x80000000;
        }
        
        result += static_cast<char>((dep >> 24) & 0xFF);
        result += static_cast<char>((dep >> 16) & 0xFF);
        result += static_cast<char>((dep >> 8) & 0xFF);
        result += static_cast<char>(dep & 0xFF);
        result += static_cast<char>(m_weight);
        
        return result;
    }
    
    std::expected<void, Http2Error> Http2PriorityFrame::deserializePayload(const uint8_t* data, size_t length)
    {
        if (length != 5) {
            return std::unexpected(Http2Error(kHttp2Error_FrameTooLarge));
        }
        
        uint32_t dep = (static_cast<uint32_t>(data[0]) << 24) |
                      (static_cast<uint32_t>(data[1]) << 16) |
                      (static_cast<uint32_t>(data[2]) << 8) |
                       static_cast<uint32_t>(data[3]);
        m_exclusive = (dep & 0x80000000) != 0;
        m_stream_dependency = dep & 0x7FFFFFFF;
        m_weight = data[4];
        
        return {};
    }
    
    // ==================== Http2RstStreamFrame ====================
    
    Http2RstStreamFrame::Http2RstStreamFrame()
        : m_error_code(Http2ErrorCode::NO_ERROR)
    {
        m_header.type = Http2FrameType::RST_STREAM;
        m_header.length = 4;
    }
    
    Http2RstStreamFrame::Http2RstStreamFrame(uint32_t stream_id, Http2ErrorCode error_code)
        : m_error_code(error_code)
    {
        m_header.type = Http2FrameType::RST_STREAM;
        m_header.stream_id = stream_id;
        m_header.length = 4;
    }
    
    std::string Http2RstStreamFrame::serialize() const
    {
        std::string result = m_header.serialize();
        
        uint32_t code = static_cast<uint32_t>(m_error_code);
        result += static_cast<char>((code >> 24) & 0xFF);
        result += static_cast<char>((code >> 16) & 0xFF);
        result += static_cast<char>((code >> 8) & 0xFF);
        result += static_cast<char>(code & 0xFF);
        
        return result;
    }
    
    std::expected<void, Http2Error> Http2RstStreamFrame::deserializePayload(const uint8_t* data, size_t length)
    {
        if (length != 4) {
            return std::unexpected(Http2Error(kHttp2Error_FrameTooLarge));
        }
        
        uint32_t code = (static_cast<uint32_t>(data[0]) << 24) |
                       (static_cast<uint32_t>(data[1]) << 16) |
                       (static_cast<uint32_t>(data[2]) << 8) |
                        static_cast<uint32_t>(data[3]);
        m_error_code = static_cast<Http2ErrorCode>(code);
        
        return {};
    }
    
    // ==================== Http2SettingsFrame ====================
    
    Http2SettingsFrame::Http2SettingsFrame()
    {
        m_header.type = Http2FrameType::SETTINGS;
        m_header.stream_id = 0;  // SETTINGS 必须是流 0
    }
    
    Http2SettingsFrame::Http2SettingsFrame(bool ack)
    {
        m_header.type = Http2FrameType::SETTINGS;
        m_header.stream_id = 0;
        if (ack) {
            m_header.flags = FLAG_ACK;
            m_header.length = 0;
        }
    }
    
    void Http2SettingsFrame::setSetting(Http2SettingsId id, uint32_t value)
    {
        m_settings[id] = value;
        m_header.length = m_settings.size() * 6;
    }
    
    std::expected<uint32_t, Http2Error> Http2SettingsFrame::getSetting(Http2SettingsId id) const
    {
        auto it = m_settings.find(id);
        if (it == m_settings.end()) {
            return std::unexpected(Http2Error(kHttp2Error_InvalidSettings));
        }
        return it->second;
    }
    
    std::string Http2SettingsFrame::serialize() const
    {
        std::string result = m_header.serialize();
        
        for (const auto& [id, value] : m_settings) {
            uint16_t setting_id = static_cast<uint16_t>(id);
            result += static_cast<char>((setting_id >> 8) & 0xFF);
            result += static_cast<char>(setting_id & 0xFF);
            result += static_cast<char>((value >> 24) & 0xFF);
            result += static_cast<char>((value >> 16) & 0xFF);
            result += static_cast<char>((value >> 8) & 0xFF);
            result += static_cast<char>(value & 0xFF);
        }
        
        return result;
    }
    
    std::expected<void, Http2Error> Http2SettingsFrame::deserializePayload(const uint8_t* data, size_t length)
    {
        if (length % 6 != 0) {
            return std::unexpected(Http2Error(kHttp2Error_FrameTooLarge));
        }
        
        for (size_t i = 0; i < length; i += 6) {
            uint16_t id = (static_cast<uint16_t>(data[i]) << 8) | data[i + 1];
            uint32_t value = (static_cast<uint32_t>(data[i + 2]) << 24) |
                            (static_cast<uint32_t>(data[i + 3]) << 16) |
                            (static_cast<uint32_t>(data[i + 4]) << 8) |
                             static_cast<uint32_t>(data[i + 5]);
            m_settings[static_cast<Http2SettingsId>(id)] = value;
        }
        
        return {};
    }
    
    // ==================== Http2PingFrame ====================
    
    Http2PingFrame::Http2PingFrame()
    {
        m_header.type = Http2FrameType::PING;
        m_header.stream_id = 0;  // PING 必须是流 0
        m_header.length = 8;
        std::memset(m_opaque_data, 0, 8);
    }
    
    Http2PingFrame::Http2PingFrame(const uint8_t* opaque_data, bool ack)
    {
        m_header.type = Http2FrameType::PING;
        m_header.stream_id = 0;
        m_header.length = 8;
        m_header.flags = ack ? FLAG_ACK : 0;
        std::memcpy(m_opaque_data, opaque_data, 8);
    }
    
    Http2PingFrame::Http2PingFrame(uint64_t data, bool ack)
    {
        m_header.type = Http2FrameType::PING;
        m_header.stream_id = 0;
        m_header.length = 8;
        m_header.flags = ack ? FLAG_ACK : 0;
        
        for (int i = 7; i >= 0; --i) {
            m_opaque_data[i] = data & 0xFF;
            data >>= 8;
        }
    }
    
    uint64_t Http2PingFrame::data() const
    {
        uint64_t result = 0;
        for (int i = 0; i < 8; ++i) {
            result = (result << 8) | m_opaque_data[i];
        }
        return result;
    }
    
    std::string Http2PingFrame::serialize() const
    {
        std::string result = m_header.serialize();
        result.append(reinterpret_cast<const char*>(m_opaque_data), 8);
        return result;
    }
    
    std::expected<void, Http2Error> Http2PingFrame::deserializePayload(const uint8_t* data, size_t length)
    {
        if (length != 8) {
            return std::unexpected(Http2Error(kHttp2Error_FrameTooLarge));
        }
        
        std::memcpy(m_opaque_data, data, 8);
        return {};
    }
    
    // ==================== Http2GoAwayFrame ====================
    
    Http2GoAwayFrame::Http2GoAwayFrame()
        : m_last_stream_id(0), m_error_code(Http2ErrorCode::NO_ERROR)
    {
        m_header.type = Http2FrameType::GOAWAY;
        m_header.stream_id = 0;  // GOAWAY 必须是流 0
        m_header.length = 8;
    }
    
    Http2GoAwayFrame::Http2GoAwayFrame(uint32_t last_stream_id, Http2ErrorCode error_code, const std::string& debug_data)
        : m_last_stream_id(last_stream_id), m_error_code(error_code), m_debug_data(debug_data)
    {
        m_header.type = Http2FrameType::GOAWAY;
        m_header.stream_id = 0;
        m_header.length = 8 + debug_data.size();
    }
    
    std::string Http2GoAwayFrame::serialize() const
    {
        std::string result = m_header.serialize();
        
        uint32_t sid = m_last_stream_id & 0x7FFFFFFF;
        result += static_cast<char>((sid >> 24) & 0xFF);
        result += static_cast<char>((sid >> 16) & 0xFF);
        result += static_cast<char>((sid >> 8) & 0xFF);
        result += static_cast<char>(sid & 0xFF);
        
        uint32_t code = static_cast<uint32_t>(m_error_code);
        result += static_cast<char>((code >> 24) & 0xFF);
        result += static_cast<char>((code >> 16) & 0xFF);
        result += static_cast<char>((code >> 8) & 0xFF);
        result += static_cast<char>(code & 0xFF);
        
        result += m_debug_data;
        
        return result;
    }
    
    std::expected<void, Http2Error> Http2GoAwayFrame::deserializePayload(const uint8_t* data, size_t length)
    {
        if (length < 8) {
            return std::unexpected(Http2Error(kHttp2Error_FrameTooLarge));
        }
        
        m_last_stream_id = ((static_cast<uint32_t>(data[0]) << 24) |
                           (static_cast<uint32_t>(data[1]) << 16) |
                           (static_cast<uint32_t>(data[2]) << 8) |
                            static_cast<uint32_t>(data[3])) & 0x7FFFFFFF;
        
        uint32_t code = (static_cast<uint32_t>(data[4]) << 24) |
                       (static_cast<uint32_t>(data[5]) << 16) |
                       (static_cast<uint32_t>(data[6]) << 8) |
                        static_cast<uint32_t>(data[7]);
        m_error_code = static_cast<Http2ErrorCode>(code);
        
        if (length > 8) {
            m_debug_data = std::string(reinterpret_cast<const char*>(data + 8), length - 8);
        }
        
        return {};
    }
    
    // ==================== Http2WindowUpdateFrame ====================
    
    Http2WindowUpdateFrame::Http2WindowUpdateFrame()
        : m_window_size_increment(0)
    {
        m_header.type = Http2FrameType::WINDOW_UPDATE;
        m_header.length = 4;
    }
    
    Http2WindowUpdateFrame::Http2WindowUpdateFrame(uint32_t stream_id, uint32_t window_size_increment)
        : m_window_size_increment(window_size_increment)
    {
        m_header.type = Http2FrameType::WINDOW_UPDATE;
        m_header.stream_id = stream_id;
        m_header.length = 4;
    }
    
    std::string Http2WindowUpdateFrame::serialize() const
    {
        std::string result = m_header.serialize();
        
        uint32_t inc = m_window_size_increment & 0x7FFFFFFF;
        result += static_cast<char>((inc >> 24) & 0xFF);
        result += static_cast<char>((inc >> 16) & 0xFF);
        result += static_cast<char>((inc >> 8) & 0xFF);
        result += static_cast<char>(inc & 0xFF);
        
        return result;
    }
    
    std::expected<void, Http2Error> Http2WindowUpdateFrame::deserializePayload(const uint8_t* data, size_t length)
    {
        if (length != 4) {
            return std::unexpected(Http2Error(kHttp2Error_FrameTooLarge));
        }
        
        m_window_size_increment = ((static_cast<uint32_t>(data[0]) << 24) |
                                  (static_cast<uint32_t>(data[1]) << 16) |
                                  (static_cast<uint32_t>(data[2]) << 8) |
                                   static_cast<uint32_t>(data[3])) & 0x7FFFFFFF;
        
        return {};
    }
    
    // ==================== Http2ContinuationFrame ====================
    
    Http2ContinuationFrame::Http2ContinuationFrame()
    {
        m_header.type = Http2FrameType::CONTINUATION;
    }
    
    Http2ContinuationFrame::Http2ContinuationFrame(uint32_t stream_id, const std::string& header_block, bool end_headers)
        : m_header_block(header_block)
    {
        m_header.type = Http2FrameType::CONTINUATION;
        m_header.stream_id = stream_id;
        m_header.flags = end_headers ? FLAG_END_HEADERS : 0;
        m_header.length = header_block.size();
    }
    
    std::string Http2ContinuationFrame::serialize() const
    {
        std::string result = m_header.serialize();
        result += m_header_block;
        return result;
    }
    
    std::expected<void, Http2Error> Http2ContinuationFrame::deserializePayload(const uint8_t* data, size_t length)
    {
        m_header_block = std::string(reinterpret_cast<const char*>(data), length);
        return {};
    }
}

