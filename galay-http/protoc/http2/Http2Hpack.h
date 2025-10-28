#ifndef GALAY_HTTP2_HPACK_H
#define GALAY_HTTP2_HPACK_H

#include "Http2HpackTable.h"
#include "Http2Huffman.h"
#include "Http2Error.h"
#include <string>
#include <vector>
#include <expected>

namespace galay::http
{
    /**
     * @brief HPACK 编码器
     * 
     * 实现 RFC 7541 的 HPACK 头部压缩
     */
    class HpackEncoder
    {
    public:
        /**
         * @brief 构造函数
         * @param max_dynamic_table_size 动态表最大大小，默认 4096
         */
        explicit HpackEncoder(size_t max_dynamic_table_size = 4096);
        
        /**
         * @brief 编码头部列表
         * @param headers 头部列表（名称-值对）
         * @param huffman_encode 是否使用哈夫曼编码，默认 true
         * @return 编码后的字节数组
         */
        std::string encodeHeaders(const std::vector<HpackHeaderField>& headers, 
                                  bool huffman_encode = true);
        
        /**
         * @brief 编码单个头部
         * @param name 头部名称
         * @param value 头部值
         * @param huffman_encode 是否使用哈夫曼编码
         * @return 编码后的字节数组
         */
        std::string encodeHeader(const std::string& name, const std::string& value,
                                bool huffman_encode = true);
        
        /**
         * @brief 设置动态表最大大小
         */
        void setDynamicTableMaxSize(size_t max_size);
        
        /**
         * @brief 获取动态表当前大小
         */
        size_t dynamicTableSize() const { return m_table.dynamicTableSize(); }
        
    private:
        // 编码整数（RFC 7541 5.1）
        void encodeInteger(std::string& output, uint64_t value, uint8_t prefix_bits);
        
        // 编码字符串（RFC 7541 5.2）
        void encodeString(std::string& output, const std::string& str, bool huffman_encode);
        
        // 编码索引头部字段（RFC 7541 6.1）
        void encodeIndexedHeader(std::string& output, size_t index);
        
        // 编码字面量头部字段 - 增量索引（RFC 7541 6.2.1）
        void encodeLiteralHeaderIncrementalIndexing(std::string& output, 
                                                     const std::string& name,
                                                     const std::string& value,
                                                     bool huffman_encode);
        
        // 编码字面量头部字段 - 不索引（RFC 7541 6.2.2）
        void encodeLiteralHeaderWithoutIndexing(std::string& output,
                                                const std::string& name,
                                                const std::string& value,
                                                bool huffman_encode);
        
        // 编码字面量头部字段 - 永不索引（RFC 7541 6.2.3）
        void encodeLiteralHeaderNeverIndexed(std::string& output,
                                             const std::string& name,
                                             const std::string& value,
                                             bool huffman_encode);
        
        HpackTable m_table;
        size_t m_max_dynamic_table_size;
    };
    
    /**
     * @brief HPACK 解码器
     */
    class HpackDecoder
    {
    public:
        /**
         * @brief 构造函数
         * @param max_dynamic_table_size 动态表最大大小，默认 4096
         */
        explicit HpackDecoder(size_t max_dynamic_table_size = 4096);
        
        /**
         * @brief 解码头部块
         * @param input 编码的头部块
         * @param length 数据长度
         * @return 解码后的头部列表，失败返回错误
         */
        std::expected<std::vector<HpackHeaderField>, Http2Error>
        decodeHeaders(const uint8_t* input, size_t length);
        
        /**
         * @brief 设置动态表最大大小
         */
        void setDynamicTableMaxSize(size_t max_size);
        
        /**
         * @brief 获取动态表当前大小
         */
        size_t dynamicTableSize() const { return m_table.dynamicTableSize(); }
        
    private:
        // 解码整数（RFC 7541 5.1）
        std::expected<uint64_t, Http2Error> decodeInteger(const uint8_t*& input,
                                                          const uint8_t* end,
                                                          uint8_t prefix_bits);
        
        // 解码字符串（RFC 7541 5.2）
        std::expected<std::string, Http2Error> decodeString(const uint8_t*& input,
                                                            const uint8_t* end);
        
        // 解码索引头部字段（RFC 7541 6.1）
        std::expected<HpackHeaderField, Http2Error> decodeIndexedHeader(const uint8_t*& input,
                                                                        const uint8_t* end);
        
        // 解码字面量头部字段（RFC 7541 6.2）
        std::expected<HpackHeaderField, Http2Error> decodeLiteralHeader(const uint8_t*& input,
                                                                        const uint8_t* end,
                                                                        uint8_t first_byte);
        
        // 解码动态表大小更新（RFC 7541 6.3）
        std::expected<void, Http2Error> decodeDynamicTableSizeUpdate(const uint8_t*& input,
                                                                      const uint8_t* end);
        
        HpackTable m_table;
        size_t m_max_dynamic_table_size;
    };
}

#endif // GALAY_HTTP2_HPACK_H

