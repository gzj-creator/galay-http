/**
 * @file HttpRouterSIMD.h
 * @brief HttpRouter SIMD 优化版本
 * @details 使用 SIMD 指令加速字符串比较
 */

#ifndef GALAY_HTTP_ROUTER_SIMD_H
#define GALAY_HTTP_ROUTER_SIMD_H

#include <string>
#include <cstring>

// 检测 SIMD 支持
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #define SIMD_SSE2_AVAILABLE
    #include <emmintrin.h>  // SSE2
    #ifdef __AVX2__
        #define SIMD_AVX2_AVAILABLE
        #include <immintrin.h>  // AVX2
    #endif
#elif defined(__ARM_NEON) || defined(__aarch64__)
    #define SIMD_NEON_AVAILABLE
    #include <arm_neon.h>
#endif

namespace galay::http::simd {

/**
 * @brief SIMD 加速的字符串比较工具类
 */
class SIMDStringCompare {
public:
    /**
     * @brief 比较两个字符串是否相等（SIMD 优化）
     * @param s1 字符串1
     * @param s2 字符串2
     * @param len 比较长度
     * @return 是否相等
     */
    static inline bool equals(const char* s1, const char* s2, size_t len) {
        #ifdef SIMD_AVX2_AVAILABLE
            return equals_avx2(s1, s2, len);
        #elif defined(SIMD_SSE2_AVAILABLE)
            return equals_sse2(s1, s2, len);
        #elif defined(SIMD_NEON_AVAILABLE)
            return equals_neon(s1, s2, len);
        #else
            return equals_scalar(s1, s2, len);
        #endif
    }

    /**
     * @brief 比较两个 std::string 是否相等（SIMD 优化）
     */
    static inline bool equals(const std::string& s1, const std::string& s2) {
        if (s1.size() != s2.size()) return false;
        return equals(s1.data(), s2.data(), s1.size());
    }

private:
    // ==================== SSE2 实现 (x86/x64) ====================
    #ifdef SIMD_SSE2_AVAILABLE
    static bool equals_sse2(const char* s1, const char* s2, size_t len) {
        const size_t SSE_WIDTH = 16;  // SSE2 一次处理 16 字节

        // 处理 16 字节对齐的部分
        size_t i = 0;
        for (; i + SSE_WIDTH <= len; i += SSE_WIDTH) {
            // 加载 16 字节
            __m128i v1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s1 + i));
            __m128i v2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s2 + i));

            // 比较 16 个字节
            __m128i cmp = _mm_cmpeq_epi8(v1, v2);

            // 检查是否全部相等
            int mask = _mm_movemask_epi8(cmp);
            if (mask != 0xFFFF) {
                return false;  // 有不相等的字节
            }
        }

        // 处理剩余字节（< 16 字节）
        for (; i < len; ++i) {
            if (s1[i] != s2[i]) return false;
        }

        return true;
    }
    #endif

    // ==================== AVX2 实现 (x86/x64) ====================
    #ifdef SIMD_AVX2_AVAILABLE
    static bool equals_avx2(const char* s1, const char* s2, size_t len) {
        const size_t AVX_WIDTH = 32;  // AVX2 一次处理 32 字节

        // 处理 32 字节对齐的部分
        size_t i = 0;
        for (; i + AVX_WIDTH <= len; i += AVX_WIDTH) {
            // 加载 32 字节
            __m256i v1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s1 + i));
            __m256i v2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s2 + i));

            // 比较 32 个字节
            __m256i cmp = _mm256_cmpeq_epi8(v1, v2);

            // 检查是否全部相等
            int mask = _mm256_movemask_epi8(cmp);
            if (mask != -1) {  // -1 表示全部位都是 1
                return false;
            }
        }

        // 处理剩余字节（< 32 字节）
        for (; i < len; ++i) {
            if (s1[i] != s2[i]) return false;
        }

        return true;
    }
    #endif

    // ==================== ARM NEON 实现 ====================
    #ifdef SIMD_NEON_AVAILABLE
    static bool equals_neon(const char* s1, const char* s2, size_t len) {
        const size_t NEON_WIDTH = 16;  // NEON 一次处理 16 字节

        size_t i = 0;
        for (; i + NEON_WIDTH <= len; i += NEON_WIDTH) {
            // 加载 16 字节
            uint8x16_t v1 = vld1q_u8(reinterpret_cast<const uint8_t*>(s1 + i));
            uint8x16_t v2 = vld1q_u8(reinterpret_cast<const uint8_t*>(s2 + i));

            // 比较
            uint8x16_t cmp = vceqq_u8(v1, v2);

            // 检查是否全部相等
            uint64x2_t cmp64 = vreinterpretq_u64_u8(cmp);
            uint64_t low = vgetq_lane_u64(cmp64, 0);
            uint64_t high = vgetq_lane_u64(cmp64, 1);

            if ((low & high) != 0xFFFFFFFFFFFFFFFFULL) {
                return false;
            }
        }

        // 处理剩余字节
        for (; i < len; ++i) {
            if (s1[i] != s2[i]) return false;
        }

        return true;
    }
    #endif

    // ==================== 标量实现（回退方案）====================
    static bool equals_scalar(const char* s1, const char* s2, size_t len) {
        return std::memcmp(s1, s2, len) == 0;
    }
};

/**
 * @brief SIMD 加速的路径段比较
 */
class PathSegmentCompare {
public:
    /**
     * @brief 快速比较路径段
     * @details 针对路径段的特点优化：
     *          1. 大部分路径段长度 < 32 字节
     *          2. 常见的路径段会被频繁比较
     */
    static inline bool equals(const std::string& seg1, const std::string& seg2) {
        // 快速路径：长度不同直接返回
        if (seg1.size() != seg2.size()) return false;

        size_t len = seg1.size();

        // 针对短字符串优化
        if (len <= 8) {
            // 使用 64-bit 整数比较（最多 8 字节）
            return equals_small(seg1.data(), seg2.data(), len);
        } else if (len <= 16) {
            // 使用 128-bit 比较（最多 16 字节）
            return equals_medium(seg1.data(), seg2.data(), len);
        } else {
            // 使用 SIMD 比较（> 16 字节）
            return SIMDStringCompare::equals(seg1, seg2);
        }
    }

private:
    // 小字符串优化（<= 8 字节）
    static inline bool equals_small(const char* s1, const char* s2, size_t len) {
        if (len == 0) return true;

        // 使用 64-bit 整数比较
        uint64_t v1 = 0, v2 = 0;
        std::memcpy(&v1, s1, len);
        std::memcpy(&v2, s2, len);
        return v1 == v2;
    }

    // 中等字符串优化（<= 16 字节）
    static inline bool equals_medium(const char* s1, const char* s2, size_t len) {
        #ifdef SIMD_SSE2_AVAILABLE
            // 使用 SSE2 一次性比较 16 字节
            __m128i v1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s1));
            __m128i v2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s2));
            __m128i cmp = _mm_cmpeq_epi8(v1, v2);

            // 创建掩码，只比较有效字节
            int mask = _mm_movemask_epi8(cmp);
            int valid_mask = (1 << len) - 1;
            return (mask & valid_mask) == valid_mask;
        #else
            return std::memcmp(s1, s2, len) == 0;
        #endif
    }
};

} // namespace galay::http::simd

#endif // GALAY_HTTP_ROUTER_SIMD_H
