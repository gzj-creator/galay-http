#ifndef GALAY_HTTP_CHUNK_H
#define GALAY_HTTP_CHUNK_H

#include "HttpError.h"
#include <string>
#include <vector>
#include <sys/uio.h>
#include <expected>

namespace galay::http
{

/**
 * @brief HTTP Chunk编码处理类
 * @details 提供chunk编码的解析和创建功能
 */
class Chunk
{
public:
    /**
     * @brief 从iovec中解析chunk数据
     * @param iovecs 输入的iovec数组
     * @param chunk_data 输出的chunk数据（追加方式）
     * @return std::expected<std::pair<bool, size_t>, HttpError>
     *         - pair.first: true表示读取到最后一个chunk，false表示还有更多chunk
     *         - pair.second: 消费的字节数
     *         - HttpError: 解析错误或数据不完整
     * @details 每次调用尝试解析一个或多个完整的chunk
     *          数据不完整时返回kIncomplete错误
     */
    static std::expected<std::pair<bool, size_t>, HttpError>
    fromIOVec(const std::vector<iovec>& iovecs, std::string& chunk_data);

    /**
     * @brief 创建chunk编码的数据
     * @param data 要编码的数据
     * @param is_last 是否是最后一个chunk
     * @return std::string chunk编码后的字符串
     * @details 格式：size(hex)\r\ndata\r\n
     *          最后一个chunk：0\r\n\r\n
     */
    static std::string toChunk(const std::string& data, bool is_last = false);

    /**
     * @brief 创建chunk编码的数据（从buffer）
     * @param data 要编码的数据指针
     * @param length 数据长度
     * @param is_last 是否是最后一个chunk
     * @return std::string chunk编码后的字符串
     */
    static std::string toChunk(const char* data, size_t length, bool is_last = false);

private:
    /**
     * @brief 从iovec中查找\r\n
     * @param iovecs iovec数组
     * @param start_iov 起始iovec索引
     * @param start_byte 起始字节索引
     * @param buffer 用于存储\r\n之前的数据
     * @param consumed 输出消费的字节数
     * @return true找到\r\n，false未找到
     */
    static bool findCRLF(const std::vector<iovec>& iovecs,
                        size_t start_iov,
                        size_t start_byte,
                        std::string& buffer,
                        size_t& consumed);

    /**
     * @brief 从iovec中读取指定长度的数据
     * @param iovecs iovec数组
     * @param start_iov 起始iovec索引
     * @param start_byte 起始字节索引
     * @param length 要读取的长度
     * @param output 输出buffer
     * @return 实际读取的字节数
     */
    static size_t readData(const std::vector<iovec>& iovecs,
                          size_t start_iov,
                          size_t start_byte,
                          size_t length,
                          std::string& output);

    /**
     * @brief 将数字转换为十六进制字符串
     * @param value 数值
     * @return 十六进制字符串
     */
    static std::string toHex(size_t value);
};

} // namespace galay::http

#endif // GALAY_HTTP_CHUNK_H
