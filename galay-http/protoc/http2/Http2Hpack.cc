#include "Http2Hpack.h"
#include <algorithm>
#include <cstring>

namespace galay::http2
{

// ==================== Huffman 编码表 (RFC 7541 Appendix B) ====================

struct HuffmanCode {
    uint32_t code;
    uint8_t bits;
};

// RFC 7541 Appendix B - Huffman Codes
static const HuffmanCode kHuffmanCodes[257] = {
    {0x1ff8, 13}, {0x7fffd8, 23}, {0xfffffe2, 28}, {0xfffffe3, 28},
    {0xfffffe4, 28}, {0xfffffe5, 28}, {0xfffffe6, 28}, {0xfffffe7, 28},
    {0xfffffe8, 28}, {0xffffea, 24}, {0x3ffffffc, 30}, {0xfffffe9, 28},
    {0xfffffea, 28}, {0x3ffffffd, 30}, {0xfffffeb, 28}, {0xfffffec, 28},
    {0xfffffed, 28}, {0xfffffee, 28}, {0xfffffef, 28}, {0xffffff0, 28},
    {0xffffff1, 28}, {0xffffff2, 28}, {0x3ffffffe, 30}, {0xffffff3, 28},
    {0xffffff4, 28}, {0xffffff5, 28}, {0xffffff6, 28}, {0xffffff7, 28},
    {0xffffff8, 28}, {0xffffff9, 28}, {0xffffffa, 28}, {0xffffffb, 28},
    {0x14, 6}, {0x3f8, 10}, {0x3f9, 10}, {0xffa, 12},
    {0x1ff9, 13}, {0x15, 6}, {0xf8, 8}, {0x7fa, 11},
    {0x3fa, 10}, {0x3fb, 10}, {0xf9, 8}, {0x7fb, 11},
    {0xfa, 8}, {0x16, 6}, {0x17, 6}, {0x18, 6},
    {0x0, 5}, {0x1, 5}, {0x2, 5}, {0x19, 6},
    {0x1a, 6}, {0x1b, 6}, {0x1c, 6}, {0x1d, 6},
    {0x1e, 6}, {0x1f, 6}, {0x5c, 7}, {0xfb, 8},
    {0x7ffc, 15}, {0x20, 6}, {0xffb, 12}, {0x3fc, 10},
    {0x1ffa, 13}, {0x21, 6}, {0x5d, 7}, {0x5e, 7},
    {0x5f, 7}, {0x60, 7}, {0x61, 7}, {0x62, 7},
    {0x63, 7}, {0x64, 7}, {0x65, 7}, {0x66, 7},
    {0x67, 7}, {0x68, 7}, {0x69, 7}, {0x6a, 7},
    {0x6b, 7}, {0x6c, 7}, {0x6d, 7}, {0x6e, 7},
    {0x6f, 7}, {0x70, 7}, {0x71, 7}, {0x72, 7},
    {0xfc, 8}, {0x73, 7}, {0xfd, 8}, {0x1ffb, 13},
    {0x7fff0, 19}, {0x1ffc, 13}, {0x3ffc, 14}, {0x22, 6},
    {0x7ffd, 15}, {0x3, 5}, {0x23, 6}, {0x4, 5},
    {0x24, 6}, {0x5, 5}, {0x25, 6}, {0x26, 6},
    {0x27, 6}, {0x6, 5}, {0x74, 7}, {0x75, 7},
    {0x28, 6}, {0x29, 6}, {0x2a, 6}, {0x7, 5},
    {0x2b, 6}, {0x76, 7}, {0x2c, 6}, {0x8, 5},
    {0x9, 5}, {0x2d, 6}, {0x77, 7}, {0x78, 7},
    {0x79, 7}, {0x7a, 7}, {0x7b, 7}, {0x7ffe, 15},
    {0x7fc, 11}, {0x3ffd, 14}, {0x1ffd, 13}, {0xffffffc, 28},
    {0xfffe6, 20}, {0x3fffd2, 22}, {0xfffe7, 20}, {0xfffe8, 20},
    {0x3fffd3, 22}, {0x3fffd4, 22}, {0x3fffd5, 22}, {0x7fffd9, 23},
    {0x3fffd6, 22}, {0x7fffda, 23}, {0x7fffdb, 23}, {0x7fffdc, 23},
    {0x7fffdd, 23}, {0x7fffde, 23}, {0xffffeb, 24}, {0x7fffdf, 23},
    {0xffffec, 24}, {0xffffed, 24}, {0x3fffd7, 22}, {0x7fffe0, 23},
    {0xffffee, 24}, {0x7fffe1, 23}, {0x7fffe2, 23}, {0x7fffe3, 23},
    {0x7fffe4, 23}, {0x1fffdc, 21}, {0x3fffd8, 22}, {0x7fffe5, 23},
    {0x3fffd9, 22}, {0x7fffe6, 23}, {0x7fffe7, 23}, {0xffffef, 24},
    {0x3fffda, 22}, {0x1fffdd, 21}, {0xfffe9, 20}, {0x3fffdb, 22},
    {0x3fffdc, 22}, {0x7fffe8, 23}, {0x7fffe9, 23}, {0x1fffde, 21},
    {0x7fffea, 23}, {0x3fffdd, 22}, {0x3fffde, 22}, {0xfffff0, 24},
    {0x1fffdf, 21}, {0x3fffdf, 22}, {0x7fffeb, 23}, {0x7fffec, 23},
    {0x1fffe0, 21}, {0x1fffe1, 21}, {0x3fffe0, 22}, {0x1fffe2, 21},
    {0x7fffed, 23}, {0x3fffe1, 22}, {0x7fffee, 23}, {0x7fffef, 23},
    {0xfffea, 20}, {0x3fffe2, 22}, {0x3fffe3, 22}, {0x3fffe4, 22},
    {0x7ffff0, 23}, {0x3fffe5, 22}, {0x3fffe6, 22}, {0x7ffff1, 23},
    {0x3ffffe0, 26}, {0x3ffffe1, 26}, {0xfffeb, 20}, {0x7fff1, 19},
    {0x3fffe7, 22}, {0x7ffff2, 23}, {0x3fffe8, 22}, {0x1ffffec, 25},
    {0x3ffffe2, 26}, {0x3ffffe3, 26}, {0x3ffffe4, 26}, {0x7ffffde, 27},
    {0x7ffffdf, 27}, {026}, {0xfffff1, 24}, {0x1ffffed, 25},
    {0x7fff2, 19}, {0x1fffe3, 21}, {0x3ffffe6, 26}, {0x7ffffe0, 27},
    {0x7ffffe1, 27}, {0x3ffffe7, 26}, {0x7ffffe2, 27}, {0xfffff2, 24},
    {0x1fffe4, 21}, {0x1fffe5, 21}, {0x3ffffe8, 26}, {0x3ffffe9, 26},
    {0xffffffd, 28}, {0x7ffffe3, 27}, {0x7ffffe4, 27}, {0x7ffffe5, 27},
    {0xfffec, 20}, {0xfffff3, 24}, {0xfffed, 20}, {0x1fffe6, 21},
    {0x3fffe9, 22}, {0x1fffe7, 21}, {0x1fffe8, 21}, {0x7ffff3, 23},
    {0x3fffea, 22}, {0x3fffeb, 22}, {0x1ffffee, 25}, {0x1ffffef, 25},
    {0xfffff4, 24}, {0xfffff5, 24}, {0x3ffffea, 26}, {0x7ffff4, 23},
    {0x3ffffeb, 26}, {0x7ffffe6, 27}, {0x3ffffec, 26}, {0x3ffffed, 26},
    {0x7ffffe7, 27}, {0x7ffffe8, 27}, {0x7ffffe9, 27}, {0x7ffffea, 27},
    {0x7ffffeb, 27}, {0xffffffe, 28}, {0x7ffffec, 27}, {0x7ffffed, 27},
    {0x7ffffee, 27}, {0x7ffffef, 27}, {0x7fffff0, 27}, {0x3ffffee, 26},
    {0x3fffffff, 30}  // EOS
};


// ==================== 静态表 (RFC 7541 Appendix A) ====================

static const Http2HeaderField kStaticTableEntries[] = {
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "200"},
    {":status", "204"},
    {":status", "206"},
    {":status", "304"},
    {":status", "400"},
    {":status", "404"},
    {":status", "500"},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""}
};

constexpr size_t kStaticTableSize = sizeof(kStaticTableEntries) / sizeof(kStaticTableEntries[0]);

// ==================== HpackStaticTable ====================

HpackStaticTable::HpackStaticTable()
{
    m_table.reserve(kStaticTableSize);
    for (size_t i = 0; i < kStaticTableSize; ++i) {
        m_table.push_back(kStaticTableEntries[i]);
    }
}

const HpackStaticTable& HpackStaticTable::instance()
{
    static HpackStaticTable instance;
    return instance;
}

const Http2HeaderField* HpackStaticTable::get(size_t index) const
{
    if (index == 0 || index > m_table.size()) {
        return nullptr;
    }
    return &m_table[index - 1];
}

std::pair<size_t, bool> HpackStaticTable::find(const std::string& name, const std::string& value) const
{
    size_t name_match = 0;
    for (size_t i = 0; i < m_table.size(); ++i) {
        if (m_table[i].name == name) {
            if (m_table[i].value == value) {
                return {i + 1, false};  // 完全匹配
            }
            if (name_match == 0) {
                name_match = i + 1;  // 记录第一个名称匹配
            }
        }
    }
    return {name_match, name_match != 0};  // 只匹配名称
}


// ==================== HpackDynamicTable ====================

HpackDynamicTable::HpackDynamicTable(size_t max_size)
    : m_max_size(max_size)
{
}

void HpackDynamicTable::add(const Http2HeaderField& field)
{
    size_t entry_size = field.size();
    
    // 如果条目大于最大大小，清空表
    if (entry_size > m_max_size) {
        clear();
        return;
    }
    
    // 驱逐条目直到有足够空间
    while (m_current_size + entry_size > m_max_size) {
        evict();
    }
    
    // 添加到表头
    m_table.push_front(field);
    m_current_size += entry_size;
}

const Http2HeaderField* HpackDynamicTable::get(size_t index) const
{
    if (index >= m_table.size()) {
        return nullptr;
    }
    return &m_table[index];
}

std::pair<size_t, bool> HpackDynamicTable::find(const std::string& name, const std::string& value) const
{
    size_t name_match = 0;
    for (size_t i = 0; i < m_table.size(); ++i) {
        if (m_table[i].name == name) {
            if (m_table[i].value == value) {
                return {i, false};  // 完全匹配
            }
            if (name_match == 0) {
                name_match = i + 1;  // 记录第一个名称匹配（+1 表示找到）
            }
        }
    }
    if (name_match > 0) {
        return {name_match - 1, true};  // 只匹配名称
    }
    return {0, false};  // 未找到
}

void HpackDynamicTable::setMaxSize(size_t max_size)
{
    m_max_size = max_size;
    while (m_current_size > m_max_size) {
        evict();
    }
}

void HpackDynamicTable::clear()
{
    m_table.clear();
    m_current_size = 0;
}

void HpackDynamicTable::evict()
{
    if (m_table.empty()) {
        return;
    }
    m_current_size -= m_table.back().size();
    m_table.pop_back();
}


// ==================== HpackHuffman ====================

std::string HpackHuffman::encode(const std::string& input)
{
    std::string output;
    uint64_t buffer = 0;
    int bits_in_buffer = 0;
    
    for (unsigned char c : input) {
        const HuffmanCode& code = kHuffmanCodes[c];
        buffer = (buffer << code.bits) | code.code;
        bits_in_buffer += code.bits;
        
        while (bits_in_buffer >= 8) {
            bits_in_buffer -= 8;
            output.push_back(static_cast<char>((buffer >> bits_in_buffer) & 0xFF));
        }
    }
    
    // 填充 EOS 符号的前缀
    if (bits_in_buffer > 0) {
        buffer = (buffer << (8 - bits_in_buffer)) | (0xFF >> bits_in_buffer);
        output.push_back(static_cast<char>(buffer & 0xFF));
    }
    
    return output;
}

size_t HpackHuffman::encodedLength(const std::string& input)
{
    size_t bits = 0;
    for (unsigned char c : input) {
        bits += kHuffmanCodes[c].bits;
    }
    return (bits + 7) / 8;
}

std::expected<std::string, Http2ErrorCode> HpackHuffman::decode(const uint8_t* data, size_t length)
{
    std::string output;
    uint32_t buffer = 0;
    int bits_in_buffer = 0;
    
    // 构建解码状态机（简化版本，使用位匹配）
    for (size_t i = 0; i < length; ++i) {
        buffer = (buffer << 8) | data[i];
        bits_in_buffer += 8;
        
        while (bits_in_buffer >= 5) {  // 最短编码是 5 位
            bool found = false;
            
            // 尝试匹配 Huffman 码
            for (int sym = 0; sym < 256; ++sym) {
                const HuffmanCode& code = kHuffmanCodes[sym];
                if (code.bits <= bits_in_buffer) {
                    uint32_t mask = (1u << code.bits) - 1;
                    uint32_t candidate = (buffer >> (bits_in_buffer - code.bits)) & mask;
                    if (candidate == code.code) {
                        output.push_back(static_cast<char>(sym));
                        bits_in_buffer -= code.bits;
                        found = true;
                        break;
                    }
                }
            }
            
            if (!found) {
                // 检查是否是 EOS 填充
                if (bits_in_buffer < 8) {
                    uint32_t padding = buffer & ((1u << bits_in_buffer) - 1);
                    uint32_t expected_padding = (1u << bits_in_buffer) - 1;
                    if (padding == expected_padding) {
                        return output;  // 有效的 EOS 填充
                    }
                }
                break;  // 需要更多数据或无效
            }
        }
    }
    
    // 检查剩余位是否是有效的 EOS 填充
    if (bits_in_buffer > 0 && buffer < 8) {
        uint32_t padding = buffer & ((1u << bits_in_buffer) - 1);
        uint32_t expected_padding = (1u << bits_in_buffer) - 1;
        if (padding != expected_padding) {
            return std::unexpected(Http2ErrorCode::CompressionError);
        }
    }
    
    return output;
}


// ==================== HpackEncoder ====================

HpackEncoder::HpackEncoder(size_t max_table_size)
    : m_dynamic_table(max_table_size)
{
}

void HpackEncoder::encodeInteger(uint32_t value, uint8_t prefix_bits, uint8_t prefix, std::string& output)
{
    uint8_t max_prefix = (1 << prefix_bits) - 1;
    
    if (value < max_prefix) {
        output.push_back(static_cast<char>(prefix | value));
    } else {
        output.push_back(static_cast<char>(prefix | max_prefix));
        value -= max_prefix;
        while (value >= 128) {
            output.push_back(static_cast<char>((value & 0x7F) | 0x80));
            value >>= 7;
        }
        output.push_back(static_cast<char>(value));
    }
}

void HpackEncoder::encodeString(const std::string& str, bool use_huffman, std::string& output)
{
    if (use_huffman) {
        std::string encoded = HpackHuffman::encode(str);
        encodeInteger(encoded.size(), 7, 0x80, output);  // H=1
        output.append(encoded);
    } else {
        encodeInteger(str.size(), 7, 0x00, output);  // H=0
        output.append(str);
    }
}

void HpackEncoder::encodeIndexed(size_t index, std::string& output)
{
    // 索引头部字段: 1xxxxxxx
    encodeInteger(index, 7, 0x80, output);
}

void HpackEncoder::encodeLiteralIndexed(size_t name_index, const std::string& value, std::string& output)
{
    // 带索引的字面头部字段: 01xxxxxx
    encodeInteger(name_index, 6, 0x40, output);
    encodeString(value, m_use_huffman, output);
}

void HpackEncoder::encodeLiteralIndexed(const std::string& name, const std::string& value, std::string& output)
{
    // 带索引的字面头部字段，新名称: 01000000
    output.push_back(0x40);
    encodeString(name, m_use_huffman, output);
    encodeString(value, m_use_huffman, output);
}

void HpackEncoder::encodeLiteralWithoutIndexing(size_t name_index, const std::string& value, std::string& output)
{
    // 不带索引的字面头部字段: 0000xxxx
    encodeInteger(name_index, 4, 0x00, output);
    encodeString(value, m_use_huffman, output);
}

void HpackEncoder::encodeLiteralWithoutIndexing(const std::string& name, const std::string& value, std::string& output)
{
    // 不带索引的字面头部字段，新名称: 00000000
    output.push_back(0x00);
    encodeString(name, m_use_huffman, output);
    encodeString(value, m_use_huffman, output);
}

void HpackEncoder::encodeLiteralNeverIndexed(size_t name_index, const std::string& value, std::string& output)
{
    // 永不索引的字面头部字段: 0001xxxx
    encodeInteger(name_index, 4, 0x10, output);
    encodeString(value, m_use_huffman, output);
}

void HpackEncoder::encodeLiteralNeverIndexed(const std::string& name, const std::string& value, std::string& output)
{
    // 永不索引的字面头部字段，新名称: 00010000
    output.push_back(0x10);
    encodeString(name, m_use_huffman, output);
    encodeString(value, m_use_huffman, output);
}

void HpackEncoder::setMaxTableSize(size_t size)
{
    m_dynamic_table.setMaxSize(size);
    m_table_size_update_pending = true;
    m_pending_table_size = size;
}


void HpackEncoder::encodeField(const Http2HeaderField& field, std::string& output)
{
    const auto& static_table = HpackStaticTable::instance();
    
    // 首先在静态表中查找
    auto [static_idx, static_name_only] = static_table.find(field.name, field.value);
    
    if (static_idx > 0 && !static_name_only) {
        // 静态表完全匹配
        encodeIndexed(static_idx, output);
        return;
    }
    
    // 在动态表中查找
    auto [dyn_idx, dyn_name_only] = m_dynamic_table.find(field.name, field.value);
    
    if (!dyn_name_only && m_dynamic_table.get(dyn_idx) != nullptr) {
        // 动态表完全匹配
        size_t index = static_table.size() + dyn_idx + 1;
        encodeIndexed(index, output);
        return;
    }
    
    // 需要字面编码
    // 敏感头部不索引
    bool sensitive = (field.name == "authorization" || 
                      field.name == "cookie" ||
                      field.name == "set-cookie" ||
                      field.name == "proxy-authorization");
    
    if (sensitive) {
        if (static_idx > 0) {
            encodeLiteralNeverIndexed(static_idx, field.value, output);
        } else {
            encodeLiteralNeverIndexed(field.name, field.value, output);
        }
    } else {
        // 添加到动态表
        if (static_idx > 0) {
            encodeLiteralIndexed(static_idx, field.value, output);
        } else if (!dyn_name_only && dyn_idx < m_dynamic_table.count()) {
            size_t index = static_table.size() + dyn_idx + 1;
            encodeLiteralIndexed(index, field.value, output);
        } else {
            encodeLiteralIndexed(field.name, field.value, output);
        }
        m_dynamic_table.add(field);
    }
}

std::string HpackEncoder::encode(const std::vector<Http2HeaderField>& headers)
{
    std::string output;
    
    // 如果有待处理的表大小更新
    if (m_table_size_update_pending) {
        // 动态表大小更新: 001xxxxx
        encodeInteger(m_pending_table_size, 5, 0x20, output);
        m_table_size_update_pending = false;
    }
    
    for (const auto& field : headers) {
        encodeField(field, output);
    }
    
    return output;
}


// ==================== HpackDecoder ====================

HpackDecoder::HpackDecoder(size_t max_table_size)
    : m_dynamic_table(max_table_size)
    , m_max_table_size(max_table_size)
{
}

std::expected<uint32_t, Http2ErrorCode> HpackDecoder::decodeInteger(const uint8_t*& data, const uint8_t* end, uint8_t prefix_bits)
{
    if (data >= end) {
        return std::unexpected(Http2ErrorCode::CompressionError);
    }
    
    uint8_t max_prefix = (1 << prefix_bits) - 1;
    uint32_t value = (*data++) & max_prefix;
    
    if (value < max_prefix) {
        return value;
    }
    
    uint32_t m = 0;
    while (data < end) {
        uint8_t b = *data++;
        value += (b & 0x7F) << m;
        m += 7;
        
        if ((b & 0x80) == 0) {
            return value;
        }
        
        if (m > 28) {
            return std::unexpected(Http2ErrorCode::CompressionError);
        }
    }
    
    return std::unexpected(Http2ErrorCode::CompressionError);
}

std::expected<std::string, Http2ErrorCode> HpackDecoder::decodeString(const uint8_t*& data, const uint8_t* end)
{
    if (data >= end) {
        return std::unexpected(Http2ErrorCode::CompressionError);
    }
    
    bool huffman = (*data & 0x80) != 0;
    auto length_result = decodeInteger(data, end, 7);
    if (!length_result) {
        return std::unexpected(length_result.error());
    }
    
    uint32_t length = *length_result;
    if (data + length > end) {
        return std::unexpected(Http2ErrorCode::CompressionError);
    }
    
    std::string result;
    if (huffman) {
        auto decode_result = HpackHuffman::decode(data, length);
        if (!decode_result) {
            return std::unexpected(decode_result.error());
        }
        result = std::move(*decode_result);
    } else {
        result.assign(reinterpret_cast<const char*>(data), length);
    }
    
    data += length;
    return result;
}

const Http2HeaderField* HpackDecoder::getField(size_t index) const
{
    const auto& static_table = HpackStaticTable::instance();
    
    if (index == 0) {
        return nullptr;
    }
    
    if (index <= static_table.size()) {
        return static_table.get(index);
    }
    
    size_t dyn_index = index - static_table.size() - 1;
    return m_dynamic_table.get(dyn_index);
}

void HpackDecoder::setMaxTableSize(size_t size)
{
    m_max_table_size = size;
    m_dynamic_table.setMaxSize(size);
}


std::expected<std::vector<Http2HeaderField>, Http2ErrorCode> HpackDecoder::decode(const uint8_t* data, size_t length)
{
    std::vector<Http2HeaderField> headers;
    const uint8_t* end = data + length;
    
    while (data < end) {
        uint8_t byte = *data;
        
        if (byte & 0x80) {
            // 索引头部字段: 1xxxxxxx
            auto index_result = decodeInteger(data, end, 7);
            if (!index_result) {
                return std::unexpected(index_result.error());
            }
            
            const Http2HeaderField* field = getField(*index_result);
            if (!field) {
                return std::unexpected(Http2ErrorCode::CompressionError);
            }
            
            headers.push_back(*field);
        }
        else if (byte & 0x40) {
            // 带索引的字面头部字段: 01xxxxxx
            auto index_result = decodeInteger(data, end, 6);
            if (!index_result) {
                return std::unexpected(index_result.error());
            }
            
            std::string name;
            if (*index_result > 0) {
                const Http2HeaderField* field = getField(*index_result);
                if (!field) {
                    return std::unexpected(Http2ErrorCode::CompressionError);
                }
                name = field->name;
            } else {
                auto name_result = decodeString(data, end);
                if (!name_result) {
                    return std::unexpected(name_result.error());
                }
                name = std::move(*name_result);
            }
            
            auto value_result = decodeString(data, end);
            if (!value_result) {
                return std::unexpected(value_result.error());
            }
            
            Http2HeaderField field{std::move(name), std::move(*value_result)};
            m_dynamic_table.add(field);
            headers.push_back(std::move(field));
        }
        else if (byte & 0x20) {
            // 动态表大小更新: 001xxxxx
            auto size_result = decodeInteger(data, end, 5);
            if (!size_result) {
                return std::unexpected(size_result.error());
            }
            
            if (*size_result > m_max_table_size) {
                return std::unexpected(Http2ErrorCode::CompressionError);
            }
            
            m_dynamic_table.setMaxSize(*size_result);
        }
        else {
            // 不带索引或永不索引的字面头部字段: 0000xxxx 或 0001xxxx
            bool never_index = (byte & 0x10) != 0;
            (void)never_index;  // 解码时不需要区分
            
            auto index_result = decodeInteger(data, end, 4);
            if (!index_result) {
                return std::unexpected(index_result.error());
            }
            
            std::string name;
            if (*index_result > 0) {
                const Http2HeaderField* field = getField(*index_result);
                if (!field) {
                    return std::unexpected(Http2ErrorCode::CompressionError);
                }
                name = field->name;
            } else {
                auto name_result = decodeString(data, end);
                if (!name_result) {
                    return std::unexpected(name_result.error());
                }
                name = std::move(*name_result);
            }
            
            auto value_result = decodeString(data, end);
            if (!value_result) {
                return std::unexpected(value_result.error());
            }
            
            headers.push_back({std::move(name), std::move(*value_result)});
            // 不添加到动态表
        }
    }
    
    return headers;
}

} // namespace galay::http2
