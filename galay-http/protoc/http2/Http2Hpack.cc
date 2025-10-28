#include "Http2Hpack.h"
#include <algorithm>
#include <cstring>

namespace galay::http
{
    // ==================== HpackEncoder ====================
    
    HpackEncoder::HpackEncoder(size_t max_dynamic_table_size)
        : m_table(max_dynamic_table_size)
        , m_max_dynamic_table_size(max_dynamic_table_size)
    {
    }
    
    std::string HpackEncoder::encodeHeaders(const std::vector<HpackHeaderField>& headers, 
                                            bool huffman_encode)
    {
        std::string output;
        
        for (const auto& header : headers) {
            output += encodeHeader(header.name, header.value, huffman_encode);
        }
        
        return output;
    }
    
    std::string HpackEncoder::encodeHeader(const std::string& name, const std::string& value,
                                          bool huffman_encode)
    {
        std::string output;
        
        // 尝试完全匹配（名称和值）
        size_t exact_index = m_table.findExactMatch(name, value);
        if (exact_index != 0) {
            // 使用索引头部字段
            encodeIndexedHeader(output, exact_index);
            return output;
        }
        
        // 尝试名称匹配
        size_t name_index = m_table.findNameMatch(name);
        
        // 决定是否添加到动态表
        // 简单策略：敏感头部（如 authorization, cookie）不索引
        bool never_indexed = (name == "authorization" || name == "cookie" || 
                             name == "set-cookie" || name == "proxy-authorization");
        
        if (never_indexed) {
            encodeLiteralHeaderNeverIndexed(output, name, value, huffman_encode);
        } else {
            // 使用增量索引
            encodeLiteralHeaderIncrementalIndexing(output, name, value, huffman_encode);
            m_table.add(name, value);
        }
        
        return output;
    }
    
    void HpackEncoder::setDynamicTableMaxSize(size_t max_size)
    {
        m_max_dynamic_table_size = max_size;
        m_table.setDynamicTableMaxSize(max_size);
    }
    
    void HpackEncoder::encodeInteger(std::string& output, uint64_t value, uint8_t prefix_bits)
    {
        uint64_t max_prefix = (1ULL << prefix_bits) - 1;
        
        if (value < max_prefix) {
            // 直接编码
            output.back() |= static_cast<uint8_t>(value);
        } else {
            // 多字节编码
            output.back() |= static_cast<uint8_t>(max_prefix);
            value -= max_prefix;
            
            while (value >= 128) {
                output.push_back(static_cast<uint8_t>((value % 128) + 128));
                value /= 128;
            }
            output.push_back(static_cast<uint8_t>(value));
        }
    }
    
    void HpackEncoder::encodeString(std::string& output, const std::string& str, bool huffman_encode)
    {
        if (huffman_encode) {
            // 哈夫曼编码
            std::string encoded = Http2Huffman::encode(str);
            output.push_back(0x80);  // H=1 标志
            encodeInteger(output, encoded.size(), 7);
            output += encoded;
        } else {
            // 字面量
            output.push_back(0x00);  // H=0 标志
            encodeInteger(output, str.size(), 7);
            output += str;
        }
    }
    
    void HpackEncoder::encodeIndexedHeader(std::string& output, size_t index)
    {
        // 6.1: 索引头部字段
        // 1xxxxxxx
        output.push_back(0x80);
        encodeInteger(output, index, 7);
    }
    
    void HpackEncoder::encodeLiteralHeaderIncrementalIndexing(std::string& output,
                                                               const std::string& name,
                                                               const std::string& value,
                                                               bool huffman_encode)
    {
        // 6.2.1: 字面量头部字段 - 增量索引
        // 01xxxxxx
        size_t name_index = m_table.findNameMatch(name);
        
        if (name_index != 0) {
            // 使用名称索引
            output.push_back(0x40);
            encodeInteger(output, name_index, 6);
        } else {
            // 名称字面量
            output.push_back(0x40);
            encodeInteger(output, 0, 6);
            encodeString(output, name, huffman_encode);
        }
        
        // 值字面量
        encodeString(output, value, huffman_encode);
    }
    
    void HpackEncoder::encodeLiteralHeaderWithoutIndexing(std::string& output,
                                                          const std::string& name,
                                                          const std::string& value,
                                                          bool huffman_encode)
    {
        // 6.2.2: 字面量头部字段 - 不索引
        // 0000xxxx
        size_t name_index = m_table.findNameMatch(name);
        
        if (name_index != 0) {
            output.push_back(0x00);
            encodeInteger(output, name_index, 4);
        } else {
            output.push_back(0x00);
            encodeInteger(output, 0, 4);
            encodeString(output, name, huffman_encode);
        }
        
        encodeString(output, value, huffman_encode);
    }
    
    void HpackEncoder::encodeLiteralHeaderNeverIndexed(std::string& output,
                                                       const std::string& name,
                                                       const std::string& value,
                                                       bool huffman_encode)
    {
        // 6.2.3: 字面量头部字段 - 永不索引
        // 0001xxxx
        size_t name_index = m_table.findNameMatch(name);
        
        if (name_index != 0) {
            output.push_back(0x10);
            encodeInteger(output, name_index, 4);
        } else {
            output.push_back(0x10);
            encodeInteger(output, 0, 4);
            encodeString(output, name, huffman_encode);
        }
        
        encodeString(output, value, huffman_encode);
    }
    
    // ==================== HpackDecoder ====================
    
    HpackDecoder::HpackDecoder(size_t max_dynamic_table_size)
        : m_table(max_dynamic_table_size)
        , m_max_dynamic_table_size(max_dynamic_table_size)
    {
    }
    
    std::expected<std::vector<HpackHeaderField>, Http2Error>
    HpackDecoder::decodeHeaders(const uint8_t* input, size_t length)
    {
        std::vector<HpackHeaderField> headers;
        const uint8_t* end = input + length;
        
        while (input < end) {
            uint8_t first_byte = *input;
            
            if ((first_byte & 0x80) != 0) {
                // 1xxxxxxx: 索引头部字段
                auto result = decodeIndexedHeader(input, end);
                if (!result.has_value()) {
                    return std::unexpected(result.error());
                }
                headers.push_back(result.value());
            } else if ((first_byte & 0x40) != 0) {
                // 01xxxxxx: 字面量头部字段 - 增量索引
                auto result = decodeLiteralHeader(input, end, first_byte);
                if (!result.has_value()) {
                    return std::unexpected(result.error());
                }
                headers.push_back(result.value());
                m_table.add(result.value().name, result.value().value);
            } else if ((first_byte & 0x20) != 0) {
                // 001xxxxx: 动态表大小更新
                auto result = decodeDynamicTableSizeUpdate(input, end);
                if (!result.has_value()) {
                    return std::unexpected(result.error());
                }
            } else {
                // 0000xxxx or 0001xxxx: 字面量头部字段 - 不索引/永不索引
                auto result = decodeLiteralHeader(input, end, first_byte);
                if (!result.has_value()) {
                    return std::unexpected(result.error());
                }
                headers.push_back(result.value());
            }
        }
        
        return headers;
    }
    
    void HpackDecoder::setDynamicTableMaxSize(size_t max_size)
    {
        m_max_dynamic_table_size = max_size;
        m_table.setDynamicTableMaxSize(max_size);
    }
    
    std::expected<uint64_t, Http2Error> HpackDecoder::decodeInteger(const uint8_t*& input,
                                                                     const uint8_t* end,
                                                                     uint8_t prefix_bits)
    {
        if (input >= end) {
            return std::unexpected(Http2Error(kHttp2Error_CompressionError, 
                "Unexpected end of input while decoding integer"));
        }
        
        uint64_t max_prefix = (1ULL << prefix_bits) - 1;
        uint64_t value = (*input++) & max_prefix;
        
        if (value < max_prefix) {
            return value;
        }
        
        // 多字节解码
        uint64_t m = 0;
        uint8_t byte;
        
        do {
            if (input >= end) {
                return std::unexpected(Http2Error(kHttp2Error_CompressionError,
                    "Unexpected end of input while decoding integer"));
            }
            
            byte = *input++;
            value += static_cast<uint64_t>(byte & 0x7F) << m;
            m += 7;
            
            if (m > 63) {
                return std::unexpected(Http2Error(kHttp2Error_CompressionError,
                    "Integer overflow"));
            }
        } while ((byte & 0x80) != 0);
        
        return value;
    }
    
    std::expected<std::string, Http2Error> HpackDecoder::decodeString(const uint8_t*& input,
                                                                       const uint8_t* end)
    {
        if (input >= end) {
            return std::unexpected(Http2Error(kHttp2Error_CompressionError,
                "Unexpected end of input while decoding string"));
        }
        
        bool huffman_encoded = (*input & 0x80) != 0;
        auto length_result = decodeInteger(input, end, 7);
        
        if (!length_result.has_value()) {
            return std::unexpected(length_result.error());
        }
        
        uint64_t length = length_result.value();
        
        if (input + length > end) {
            return std::unexpected(Http2Error(kHttp2Error_CompressionError,
                "String length exceeds available data"));
        }
        
        if (huffman_encoded) {
            // 哈夫曼解码
            auto decoded = Http2Huffman::decode(input, length);
            if (!decoded.has_value()) {
                return std::unexpected(decoded.error());
            }
            input += length;
            return decoded.value();
        } else {
            // 字面量
            std::string result(reinterpret_cast<const char*>(input), length);
            input += length;
            return result;
        }
    }
    
    std::expected<HpackHeaderField, Http2Error> HpackDecoder::decodeIndexedHeader(const uint8_t*& input,
                                                                                   const uint8_t* end)
    {
        auto index_result = decodeInteger(input, end, 7);
        if (!index_result.has_value()) {
            return std::unexpected(index_result.error());
        }
        
        uint64_t index = index_result.value();
        if (index == 0) {
            return std::unexpected(Http2Error(kHttp2Error_CompressionError,
                "Invalid indexed header field index 0"));
        }
        
        auto field = m_table.get(static_cast<size_t>(index));
        if (!field.has_value()) {
            return std::unexpected(Http2Error(kHttp2Error_CompressionError,
                "Invalid indexed header field index"));
        }
        
        return field.value();
    }
    
    std::expected<HpackHeaderField, Http2Error> HpackDecoder::decodeLiteralHeader(const uint8_t*& input,
                                                                                   const uint8_t* end,
                                                                                   uint8_t first_byte)
    {
        uint8_t prefix_bits;
        
        if ((first_byte & 0x40) != 0) {
            // 01xxxxxx: 增量索引
            prefix_bits = 6;
        } else if ((first_byte & 0x10) != 0) {
            // 0001xxxx: 永不索引
            prefix_bits = 4;
        } else {
            // 0000xxxx: 不索引
            prefix_bits = 4;
        }
        
        auto name_index_result = decodeInteger(input, end, prefix_bits);
        if (!name_index_result.has_value()) {
            return std::unexpected(name_index_result.error());
        }
        
        uint64_t name_index = name_index_result.value();
        std::string name;
        
        if (name_index != 0) {
            // 使用索引的名称
            auto field = m_table.get(static_cast<size_t>(name_index));
            if (!field.has_value()) {
                return std::unexpected(Http2Error(kHttp2Error_CompressionError,
                    "Invalid name index in literal header field"));
            }
            name = field.value().name;
        } else {
            // 字面量名称
            auto name_result = decodeString(input, end);
            if (!name_result.has_value()) {
                return std::unexpected(name_result.error());
            }
            name = name_result.value();
        }
        
        // 解码值
        auto value_result = decodeString(input, end);
        if (!value_result.has_value()) {
            return std::unexpected(value_result.error());
        }
        
        return HpackHeaderField(name, value_result.value());
    }
    
    std::expected<void, Http2Error> HpackDecoder::decodeDynamicTableSizeUpdate(const uint8_t*& input,
                                                                                const uint8_t* end)
    {
        auto size_result = decodeInteger(input, end, 5);
        if (!size_result.has_value()) {
            return std::unexpected(size_result.error());
        }
        
        uint64_t new_size = size_result.value();
        
        if (new_size > m_max_dynamic_table_size) {
            return std::unexpected(Http2Error(kHttp2Error_CompressionError,
                "Dynamic table size update exceeds maximum"));
        }
        
        m_table.setDynamicTableMaxSize(static_cast<size_t>(new_size));
        return {};
    }
}

