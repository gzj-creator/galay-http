#include "WebSocketFrame.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <random>

// SIMD 支持检测
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #include <immintrin.h>
    #define GALAY_WS_SIMD_X86
#elif defined(__ARM_NEON) || defined(__aarch64__)
    #include <arm_neon.h>
    #define GALAY_WS_SIMD_NEON
#endif

namespace galay::websocket
{

namespace {

class IovecCursor {
public:
    IovecCursor(const std::vector<iovec>& iovecs, size_t total_length)
        : m_iovecs(iovecs)
        , m_total_length(total_length)
        , m_iov_index(0)
        , m_iov_offset(0)
        , m_consumed(0)
    {
    }

    size_t consumed() const {
        return m_consumed;
    }

    size_t remaining() const {
        return m_total_length - m_consumed;
    }

    bool readByte(uint8_t& out) {
        return readBytes(&out, 1);
    }

    bool readBytes(void* dst, size_t len) {
        uint8_t* out = static_cast<uint8_t*>(dst);
        size_t need = len;

        while (need > 0) {
            if (m_iov_index >= m_iovecs.size()) {
                return false;
            }

            const auto& iov = m_iovecs[m_iov_index];
            if (m_iov_offset >= iov.iov_len) {
                ++m_iov_index;
                m_iov_offset = 0;
                continue;
            }

            const size_t available = iov.iov_len - m_iov_offset;
            const size_t take = std::min(available, need);

            std::memcpy(out,
                       static_cast<const uint8_t*>(iov.iov_base) + m_iov_offset,
                       take);

            out += take;
            need -= take;
            m_iov_offset += take;
            m_consumed += take;

            if (m_iov_offset == iov.iov_len) {
                ++m_iov_index;
                m_iov_offset = 0;
            }
        }

        return true;
    }

private:
    const std::vector<iovec>& m_iovecs;
    size_t m_total_length;
    size_t m_iov_index;
    size_t m_iov_offset;
    size_t m_consumed;
};

} // namespace

std::expected<size_t, WsError>
WsFrameParser::fromIOVec(const std::vector<iovec>& iovecs, WsFrame& frame, bool is_server)
{
    const size_t total_length = getTotalLength(iovecs);
    if (total_length < 2) {
        return std::unexpected(WsError(kWsIncomplete));
    }

    IovecCursor cursor(iovecs, total_length);
    uint8_t byte1 = 0;
    uint8_t byte2 = 0;

    if (!cursor.readByte(byte1)) {
        return std::unexpected(WsError(kWsIncomplete));
    }

    // 解析FIN和RSV位
    frame.header.fin = (byte1 & 0x80) != 0;
    frame.header.rsv1 = (byte1 & 0x40) != 0;
    frame.header.rsv2 = (byte1 & 0x20) != 0;
    frame.header.rsv3 = (byte1 & 0x10) != 0;

    // 检查保留位（如果没有协商扩展，保留位必须为0）
    if (frame.header.rsv1 || frame.header.rsv2 || frame.header.rsv3) {
        return std::unexpected(WsError(kWsReservedBitsSet));
    }

    // 解析操作码
    uint8_t opcode_value = byte1 & 0x0F;
    if (opcode_value > 0x0A || (opcode_value > 0x02 && opcode_value < 0x08)) {
        return std::unexpected(WsError(kWsInvalidOpcode));
    }
    frame.header.opcode = static_cast<WsOpcode>(opcode_value);

    // 控制帧必须设置FIN位
    if (isControlFrame(frame.header.opcode) && !frame.header.fin) {
        return std::unexpected(WsError(kWsControlFrameFragmented));
    }

    if (!cursor.readByte(byte2)) {
        return std::unexpected(WsError(kWsIncomplete));
    }

    // 解析MASK位
    frame.header.mask = (byte2 & 0x80) != 0;

    // 服务器端要求客户端必须使用掩码
    if (is_server && !frame.header.mask) {
        return std::unexpected(WsError(kWsMaskRequired));
    }

    // 客户端不应该收到带掩码的帧
    if (!is_server && frame.header.mask) {
        return std::unexpected(WsError(kWsMaskNotAllowed));
    }

    // 解析payload长度
    uint8_t payload_len = byte2 & 0x7F;

    if (payload_len < 126) {
        frame.header.payload_length = payload_len;
    } else if (payload_len == 126) {
        uint8_t len_buf[2];
        if (!cursor.readBytes(len_buf, sizeof(len_buf))) {
            return std::unexpected(WsError(kWsIncomplete));
        }
        const uint16_t extended_len = (static_cast<uint16_t>(len_buf[0]) << 8) | len_buf[1];
        frame.header.payload_length = extended_len;
    } else {
        uint8_t len_buf[8];
        if (!cursor.readBytes(len_buf, sizeof(len_buf))) {
            return std::unexpected(WsError(kWsIncomplete));
        }
        uint64_t extended_len = 0;
        for (size_t i = 0; i < sizeof(len_buf); ++i) {
            extended_len = (extended_len << 8) | len_buf[i];
        }
        frame.header.payload_length = extended_len;
    }

    // 控制帧的payload不能超过125字节
    if (isControlFrame(frame.header.opcode) && frame.header.payload_length > 125) {
        return std::unexpected(WsError(kWsControlFrameTooLarge));
    }

    // 读取掩码密钥（如果有）
    if (frame.header.mask) {
        if (!cursor.readBytes(frame.header.masking_key, sizeof(frame.header.masking_key))) {
            return std::unexpected(WsError(kWsIncomplete));
        }
    }

    if (cursor.remaining() < frame.header.payload_length) {
        return std::unexpected(WsError(kWsIncomplete));
    }

    if (frame.header.payload_length > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return std::unexpected(WsError(kWsInvalidFrame));
    }
    const size_t payload_size = static_cast<size_t>(frame.header.payload_length);

    frame.payload.clear();
    frame.payload.resize(payload_size);
    if (payload_size > 0 && !cursor.readBytes(frame.payload.data(), payload_size)) {
        return std::unexpected(WsError(kWsInvalidFrame));
    }

    // 如果有掩码，解除掩码
    if (frame.header.mask) {
        applyMask(frame.payload, frame.header.masking_key);
    }

    // 验证文本帧的UTF-8编码
    if (frame.header.opcode == WsOpcode::Text && frame.header.fin) {
        if (!isValidUtf8(frame.payload)) {
            return std::unexpected(WsError(kWsInvalidUtf8));
        }
    }

    return cursor.consumed();
}

std::string WsFrameParser::toBytes(const WsFrame& frame, bool use_mask)
{
    std::string result;
    const uint64_t payload_len = frame.payload.size();
    size_t header_len = 2;
    if (payload_len >= 126 && payload_len <= 0xFFFF) {
        header_len += 2;
    } else if (payload_len > 0xFFFF) {
        header_len += 8;
    }
    if (use_mask) {
        header_len += 4;
    }
    result.reserve(header_len + static_cast<size_t>(payload_len));

    // 第一个字节: FIN + RSV + Opcode
    uint8_t byte1 = 0;
    if (frame.header.fin) byte1 |= 0x80;
    if (frame.header.rsv1) byte1 |= 0x40;
    if (frame.header.rsv2) byte1 |= 0x20;
    if (frame.header.rsv3) byte1 |= 0x10;
    byte1 |= static_cast<uint8_t>(frame.header.opcode) & 0x0F;
    result.push_back(byte1);

    // 第二个字节: MASK + Payload length
    uint8_t byte2 = 0;
    if (use_mask) byte2 |= 0x80;

    if (payload_len < 126) {
        byte2 |= static_cast<uint8_t>(payload_len);
        result.push_back(byte2);
    } else if (payload_len <= 0xFFFF) {
        byte2 |= 126;
        result.push_back(byte2);
        // 16位扩展长度（大端序）
        result.push_back((payload_len >> 8) & 0xFF);
        result.push_back(payload_len & 0xFF);
    } else {
        byte2 |= 127;
        result.push_back(byte2);
        // 64位扩展长度（大端序）
        for (int i = 7; i >= 0; --i) {
            result.push_back((payload_len >> (i * 8)) & 0xFF);
        }
    }

    // 掩码密钥
    uint8_t masking_key[4] = {0, 0, 0, 0};
    if (use_mask) {
        // 生成随机掩码
        thread_local static std::random_device rd;
        thread_local static std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 255);
        for (int i = 0; i < 4; ++i) {
            masking_key[i] = dis(gen);
            result.push_back(masking_key[i]);
        }
    }

    // Payload数据
    if (!use_mask) {
        result.append(frame.payload);
    } else {
        std::string payload = frame.payload;
        applyMask(payload, masking_key);
        result += payload;
    }

    return result;
}

std::string WsFrameParser::toBytesHeader(const WsFrame& frame, bool use_mask, uint8_t masking_key[4])
{
    std::string result;
    const uint64_t payload_len = frame.payload.size();
    size_t header_len = 2;
    if (payload_len >= 126 && payload_len <= 0xFFFF) {
        header_len += 2;
    } else if (payload_len > 0xFFFF) {
        header_len += 8;
    }
    if (use_mask) {
        header_len += 4;
    }
    result.reserve(header_len);

    // 第一个字节: FIN + RSV + Opcode
    uint8_t byte1 = 0;
    if (frame.header.fin) byte1 |= 0x80;
    if (frame.header.rsv1) byte1 |= 0x40;
    if (frame.header.rsv2) byte1 |= 0x20;
    if (frame.header.rsv3) byte1 |= 0x10;
    byte1 |= static_cast<uint8_t>(frame.header.opcode) & 0x0F;
    result.push_back(byte1);

    // 第二个字节: MASK + Payload length
    uint8_t byte2 = 0;
    if (use_mask) byte2 |= 0x80;

    if (payload_len < 126) {
        byte2 |= static_cast<uint8_t>(payload_len);
        result.push_back(byte2);
    } else if (payload_len <= 0xFFFF) {
        byte2 |= 126;
        result.push_back(byte2);
        // 16位扩展长度（大端序）
        result.push_back((payload_len >> 8) & 0xFF);
        result.push_back(payload_len & 0xFF);
    } else {
        byte2 |= 127;
        result.push_back(byte2);
        // 64位扩展长度（大端序）
        for (int i = 7; i >= 0; --i) {
            result.push_back((payload_len >> (i * 8)) & 0xFF);
        }
    }

    // 掩码密钥
    if (use_mask) {
        // 生成随机掩码
        thread_local static std::random_device rd;
        thread_local static std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 255);
        for (int i = 0; i < 4; ++i) {
            masking_key[i] = dis(gen);
            result.push_back(masking_key[i]);
        }
    }

    return result;
}

WsFrame WsFrameParser::createCloseFrame(WsCloseCode code, const std::string& reason)
{
    std::string payload;
    // 关闭码（大端序）
    payload.push_back((static_cast<uint16_t>(code) >> 8) & 0xFF);
    payload.push_back(static_cast<uint16_t>(code) & 0xFF);
    // 关闭原因
    payload += reason;

    return WsFrame(WsOpcode::Close, payload, true);
}

void WsFrameParser::applyMask(std::string& data, const uint8_t masking_key[4])
{
    size_t len = data.size();
    if (len == 0) return;

    uint8_t* ptr = reinterpret_cast<uint8_t*>(data.data());
    size_t i = 0;

#if defined(GALAY_WS_SIMD_NEON)
    // ARM NEON 优化：一次处理 16 字节
    if (len >= 16) {
        // 构造 16 字节的掩码向量 (重复 4 次 4 字节掩码)
        uint8_t mask_array[16];
        for (int j = 0; j < 16; j++) {
            mask_array[j] = masking_key[j % 4];
        }
        uint8x16_t mask_vec = vld1q_u8(mask_array);

        // 处理 16 字节对齐的块
        for (; i + 16 <= len; i += 16) {
            uint8x16_t data_vec = vld1q_u8(ptr + i);
            uint8x16_t result = veorq_u8(data_vec, mask_vec);
            vst1q_u8(ptr + i, result);
        }
    }
#elif defined(GALAY_WS_SIMD_X86)
    // x86 SSE2 优化：一次处理 16 字节
    if (len >= 16) {
        // 构造 16 字节的掩码向量
        uint8_t mask_array[16];
        for (int j = 0; j < 16; j++) {
            mask_array[j] = masking_key[j % 4];
        }
        __m128i mask_vec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(mask_array));

        // 处理 16 字节对齐的块
        for (; i + 16 <= len; i += 16) {
            __m128i data_vec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr + i));
            __m128i result = _mm_xor_si128(data_vec, mask_vec);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(ptr + i), result);
        }
    }
#endif

    // 处理 8 字节块（使用 uint64_t）
    if (i + 8 <= len) {
        // 构造 8 字节掩码
        uint64_t mask64;
        std::memcpy(&mask64, masking_key, 4);
        std::memcpy(reinterpret_cast<uint8_t*>(&mask64) + 4, masking_key, 4);

        for (; i + 8 <= len; i += 8) {
            uint64_t* data64 = reinterpret_cast<uint64_t*>(ptr + i);
            *data64 ^= mask64;
        }
    }

    // 处理 4 字节块（使用 uint32_t）
    if (i + 4 <= len) {
        uint32_t mask32;
        std::memcpy(&mask32, masking_key, 4);
        uint32_t* data32 = reinterpret_cast<uint32_t*>(ptr + i);
        *data32 ^= mask32;
        i += 4;
    }

    // 处理剩余字节
    for (; i < len; ++i) {
        ptr[i] ^= masking_key[i % 4];
    }
}

bool WsFrameParser::isValidUtf8(const std::string& data)
{
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(data.data());
    size_t len = data.size();
    size_t i = 0;

#if defined(GALAY_WS_SIMD_NEON)
    // ARM NEON 优化：快速检测 ASCII 字符（0x00-0x7F）
    if (len >= 16) {
        for (; i + 16 <= len; i += 16) {
            uint8x16_t chunk = vld1q_u8(ptr + i);
            // 检查是否所有字节都是 ASCII (最高位为 0)
            uint8x16_t high_bits = vandq_u8(chunk, vdupq_n_u8(0x80));
            // 如果有任何非 ASCII 字符，跳出 SIMD 处理
            uint64x2_t result = vreinterpretq_u64_u8(high_bits);
            if (vgetq_lane_u64(result, 0) != 0 || vgetq_lane_u64(result, 1) != 0) {
                break;
            }
        }
    }
#elif defined(GALAY_WS_SIMD_X86)
    // x86 SSE2 优化：快速检测 ASCII 字符
    if (len >= 16) {
        __m128i high_bit_mask = _mm_set1_epi8(0x80);
        for (; i + 16 <= len; i += 16) {
            __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr + i));
            __m128i high_bits = _mm_and_si128(chunk, high_bit_mask);
            // 如果有任何非 ASCII 字符，跳出 SIMD 处理
            if (_mm_movemask_epi8(high_bits) != 0) {
                break;
            }
        }
    }
#endif

    // 标量处理剩余字节和多字节 UTF-8 序列
    while (i < len) {
        uint8_t byte = ptr[i];

        if (byte <= 0x7F) {
            // 单字节字符 (0xxxxxxx)
            i++;
        } else if ((byte & 0xE0) == 0xC0) {
            // 双字节字符 (110xxxxx 10xxxxxx)
            if (i + 1 >= len) return false;
            uint8_t byte2 = ptr[i + 1];
            if ((byte2 & 0xC0) != 0x80) return false;

            // 检查过长编码：双字节序列的最小值是 0x80
            uint32_t codepoint = ((byte & 0x1F) << 6) | (byte2 & 0x3F);
            if (codepoint < 0x80) return false;

            i += 2;
        } else if ((byte & 0xF0) == 0xE0) {
            // 三字节字符 (1110xxxx 10xxxxxx 10xxxxxx)
            if (i + 2 >= len) return false;
            uint8_t byte2 = ptr[i + 1];
            uint8_t byte3 = ptr[i + 2];
            if ((byte2 & 0xC0) != 0x80) return false;
            if ((byte3 & 0xC0) != 0x80) return false;

            // 检查过长编码：三字节序列的最小值是 0x800
            uint32_t codepoint = ((byte & 0x0F) << 12) | ((byte2 & 0x3F) << 6) | (byte3 & 0x3F);
            if (codepoint < 0x800) return false;

            // 检查代理对范围 (U+D800 到 U+DFFF 是无效的)
            if (codepoint >= 0xD800 && codepoint <= 0xDFFF) return false;

            i += 3;
        } else if ((byte & 0xF8) == 0xF0) {
            // 四字节字符 (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx)
            if (i + 3 >= len) return false;
            uint8_t byte2 = ptr[i + 1];
            uint8_t byte3 = ptr[i + 2];
            uint8_t byte4 = ptr[i + 3];
            if ((byte2 & 0xC0) != 0x80) return false;
            if ((byte3 & 0xC0) != 0x80) return false;
            if ((byte4 & 0xC0) != 0x80) return false;

            // 检查过长编码：四字节序列的最小值是 0x10000
            uint32_t codepoint = ((byte & 0x07) << 18) | ((byte2 & 0x3F) << 12) |
                                ((byte3 & 0x3F) << 6) | (byte4 & 0x3F);
            if (codepoint < 0x10000) return false;

            // 检查最大值：Unicode 最大码点是 U+10FFFF
            if (codepoint > 0x10FFFF) return false;

            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

size_t WsFrameParser::getTotalLength(const std::vector<iovec>& iovecs)
{
    if (iovecs.empty()) {
        return 0;
    }
    if (iovecs.size() == 1) {
        return iovecs[0].iov_len;
    }
    if (iovecs.size() == 2) {
        return iovecs[0].iov_len + iovecs[1].iov_len;
    }

    size_t total = 0;
    for (const auto& iov : iovecs) {
        total += iov.iov_len;
    }
    return total;
}

} // namespace galay::websocket
