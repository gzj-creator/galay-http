#ifndef GALAY_HTTP2_HUFFMAN_H
#define GALAY_HTTP2_HUFFMAN_H

#include <string>
#include <cstdint>
#include <vector>
#include <expected>
#include "Http2Error.h"

namespace galay::http
{
    /**
     * @brief HTTP/2 哈夫曼编码/解码器
     * 
     * 实现 RFC 7541 附录 B 中定义的哈夫曼编码
     * 用于压缩 HPACK 中的字符串字面量
     */
    class Http2Huffman
    {
    public:
        /**
         * @brief 哈夫曼编码
         * @param input 输入字符串
         * @return 编码后的字节数组
         */
        static std::string encode(const std::string& input);
        
        /**
         * @brief 哈夫曼解码
         * @param input 编码后的字节数组
         * @param length 字节数组长度
         * @return 解码后的字符串，如果解码失败返回错误
         */
        static std::expected<std::string, Http2Error> decode(const uint8_t* input, size_t length);
        
        /**
         * @brief 计算编码后的长度
         * @param input 输入字符串
         * @return 编码后的字节数
         */
        static size_t encodedLength(const std::string& input);
        
    private:
        // 哈夫曼编码表（符号 -> (编码, 位长度)）
        struct HuffmanCode {
            uint32_t code;      // 编码值
            uint8_t bits;       // 位长度
        };
        
        // 哈夫曼解码树节点
        struct DecodeNode {
            int16_t symbol;     // -1 表示非叶子节点，>=0 表示解码出的符号
            uint16_t left;      // 左子节点索引（0 bit）
            uint16_t right;     // 右子节点索引（1 bit）
        };
        
        // RFC 7541 附录 B 定义的哈夫曼编码表
        static const HuffmanCode HUFFMAN_CODES[257];
        
        // 解码树
        static const DecodeNode DECODE_TREE[];
        static const size_t DECODE_TREE_SIZE;
    };
}

#endif // GALAY_HTTP2_HUFFMAN_H

