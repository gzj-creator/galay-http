#include "HttpChunk.h"
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace galay::http
{

std::expected<std::pair<bool, size_t>, HttpError>
Chunk::fromIOVec(const std::vector<iovec>& iovecs, std::string& chunk_data)
{
    size_t total_consumed = 0;
    size_t iov_idx = 0;
    size_t byte_idx = 0;
    bool has_parsed_chunk = false;

    // 循环解析所有可用的完整chunk
    while (iov_idx < iovecs.size()) {
        // 解析chunk size行：HEX\r\n
        std::string size_buffer;
        size_t consumed = 0;

        if (!findCRLF(iovecs, iov_idx, byte_idx, size_buffer, consumed)) {
            // 没有找到完整的size行，数据不完整
            if (has_parsed_chunk) {
                // 已经解析了至少一个chunk，返回已消费的字节数
                return std::pair<bool, size_t>{false, total_consumed};
            }
            return std::unexpected(HttpError(kIncomplete));
        }

        // 更新位置
        total_consumed += consumed;
        size_t remaining = consumed;
        while (remaining > 0 && iov_idx < iovecs.size()) {
            size_t available = iovecs[iov_idx].iov_len - byte_idx;
            if (available <= remaining) {
                remaining -= available;
                iov_idx++;
                byte_idx = 0;
            } else {
                byte_idx += remaining;
                remaining = 0;
            }
        }

        // 解析chunk size（十六进制）
        size_t chunk_size = 0;
        try {
            chunk_size = std::stoull(size_buffer, nullptr, 16);
        } catch (...) {
            return std::unexpected(HttpError(kChunkSizeConvertError));
        }

        // 检查是否是最后一个chunk
        if (chunk_size == 0) {
            // 最后一个chunk，需要消费trailing \r\n
            std::string trailing_buffer;
            size_t trailing_consumed = 0;
            if (!findCRLF(iovecs, iov_idx, byte_idx, trailing_buffer, trailing_consumed)) {
                // trailing CRLF不完整
                if (has_parsed_chunk) {
                    return std::pair<bool, size_t>{false, total_consumed};
                }
                return std::unexpected(HttpError(kIncomplete));
            }

            if (!trailing_buffer.empty()) {
                // trailing应该是空的
                return std::unexpected(HttpError(kInvalidChunkFormat));
            }

            total_consumed += trailing_consumed;
            return std::pair<bool, size_t>{true, total_consumed};
        }

        // 计算剩余可用数据
        size_t available = 0;
        for (size_t i = iov_idx; i < iovecs.size(); ++i) {
            if (i == iov_idx) {
                available += iovecs[i].iov_len - byte_idx;
            } else {
                available += iovecs[i].iov_len;
            }
        }

        // 检查是否有足够的数据（chunk data + \r\n）
        if (available < chunk_size + 2) {
            // 数据不完整
            if (has_parsed_chunk) {
                return std::pair<bool, size_t>{false, total_consumed};
            }
            return std::unexpected(HttpError(kIncomplete));
        }

        // 读取chunk data
        size_t read_bytes = readData(iovecs, iov_idx, byte_idx, chunk_size, chunk_data);
        if (read_bytes != chunk_size) {
            return std::unexpected(HttpError(kInvalidChunkFormat));
        }

        total_consumed += chunk_size;

        // 更新位置
        remaining = chunk_size;
        while (remaining > 0 && iov_idx < iovecs.size()) {
            size_t available_in_iov = iovecs[iov_idx].iov_len - byte_idx;
            if (available_in_iov <= remaining) {
                remaining -= available_in_iov;
                iov_idx++;
                byte_idx = 0;
            } else {
                byte_idx += remaining;
                remaining = 0;
            }
        }

        // 验证并消费trailing \r\n
        if (iov_idx >= iovecs.size()) {
            return std::unexpected(HttpError(kInvalidChunkFormat));
        }

        const char* data = static_cast<const char*>(iovecs[iov_idx].iov_base);
        size_t len = iovecs[iov_idx].iov_len;

        if (byte_idx + 2 <= len) {
            if (data[byte_idx] != '\r' || data[byte_idx + 1] != '\n') {
                return std::unexpected(HttpError(kInvalidChunkFormat));
            }
            byte_idx += 2;
        } else if (byte_idx + 1 == len && data[byte_idx] == '\r') {
            if (iov_idx + 1 >= iovecs.size()) {
                return std::unexpected(HttpError(kInvalidChunkFormat));
            }
            const char* next_data = static_cast<const char*>(iovecs[iov_idx + 1].iov_base);
            if (next_data[0] != '\n') {
                return std::unexpected(HttpError(kInvalidChunkFormat));
            }
            iov_idx++;
            byte_idx = 1;
        } else {
            return std::unexpected(HttpError(kInvalidChunkFormat));
        }

        total_consumed += 2;
        has_parsed_chunk = true;

        // 继续循环，尝试解析更多chunk
    }

    // 如果解析了至少一个chunk，返回false表示还有更多chunk
    if (has_parsed_chunk) {
        return std::pair<bool, size_t>{false, total_consumed};
    }

    // 没有解析到完整的chunk
    return std::unexpected(HttpError(kIncomplete));
}

std::string Chunk::toChunk(const std::string& data, bool is_last)
{
    return toChunk(data.data(), data.size(), is_last);
}

std::string Chunk::toChunk(const char* data, size_t length, bool is_last)
{
    if (is_last) {
        return "0\r\n\r\n";
    }

    std::ostringstream oss;
    oss << std::hex << length << "\r\n";
    std::string result = oss.str();
    result.append(data, length);
    result.append("\r\n");
    return result;
}

bool Chunk::findCRLF(const std::vector<iovec>& iovecs,
                     size_t start_iov,
                     size_t start_byte,
                     std::string& buffer,
                     size_t& consumed)
{
    buffer.clear();
    consumed = 0;
    size_t iov_idx = start_iov;
    size_t byte_idx = start_byte;

    while (iov_idx < iovecs.size()) {
        const char* data = static_cast<const char*>(iovecs[iov_idx].iov_base);
        size_t len = iovecs[iov_idx].iov_len;

        for (size_t i = byte_idx; i < len; ++i) {
            char c = data[i];
            consumed++;

            if (c == '\r') {
                // 检查下一个字符是否是\n
                if (i + 1 < len) {
                    if (data[i + 1] == '\n') {
                        consumed++;
                        return true;
                    }
                } else if (iov_idx + 1 < iovecs.size()) {
                    // \n在下一个iovec中
                    const char* next_data = static_cast<const char*>(iovecs[iov_idx + 1].iov_base);
                    if (next_data[0] == '\n') {
                        consumed++;
                        return true;
                    }
                } else {
                    // 数据不完整
                    return false;
                }
            } else if (c == '\n') {
                // 错误：没有\r直接\n
                return false;
            } else {
                buffer.push_back(c);
            }
        }

        iov_idx++;
        byte_idx = 0;
    }

    return false;
}

size_t Chunk::readData(const std::vector<iovec>& iovecs,
                       size_t start_iov,
                       size_t start_byte,
                       size_t length,
                       std::string& output)
{
    size_t read_bytes = 0;
    size_t iov_idx = start_iov;
    size_t byte_idx = start_byte;

    while (read_bytes < length && iov_idx < iovecs.size()) {
        const char* data = static_cast<const char*>(iovecs[iov_idx].iov_base);
        size_t len = iovecs[iov_idx].iov_len;
        size_t available = len - byte_idx;
        size_t to_read = std::min(available, length - read_bytes);

        output.append(data + byte_idx, to_read);
        read_bytes += to_read;

        if (to_read == available) {
            iov_idx++;
            byte_idx = 0;
        } else {
            byte_idx += to_read;
        }
    }

    return read_bytes;
}

} // namespace galay::http
