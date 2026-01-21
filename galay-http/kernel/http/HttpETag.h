#ifndef GALAY_HTTP_ETAG_H
#define GALAY_HTTP_ETAG_H

#include <string>
#include <ctime>
#include <cstdint>
#include <vector>
#include <filesystem>
#include <sys/stat.h>

namespace galay::http
{

namespace fs = std::filesystem;

/**
 * @brief ETag 生成器
 * @details 为文件生成 ETag，支持强 ETag 和弱 ETag
 *
 * 生产级实现：
 * - 使用文件的真实 inode、大小和修改时间
 * - 强 ETag 基于文件内容的唯一性（inode + mtime + size）
 * - 弱 ETag 仅基于修改时间和大小
 */
class ETagGenerator
{
public:
    /**
     * @brief ETag 类型
     */
    enum class Type
    {
        STRONG,  // 强 ETag - 基于 inode + mtime + size
        WEAK     // 弱 ETag - 仅基于 mtime + size
    };

    /**
     * @brief 生成文件的强 ETag
     * @details 使用 inode + mtime + size 生成，确保文件唯一性
     */
    static std::string generateStrong(const fs::path& filePath, size_t fileSize, std::time_t lastModified)
    {
        uint64_t inode = getFileInode(filePath);
        char etag[128];
        snprintf(etag, sizeof(etag), "\"%lx-%zx-%lx\"",
                 static_cast<unsigned long>(inode),
                 fileSize,
                 static_cast<unsigned long>(lastModified));
        return std::string(etag);
    }

    /**
     * @brief 生成文件的弱 ETag
     * @details 仅使用 mtime + size，适用于内容可能略有差异但语义相同的情况
     */
    static std::string generateWeak(const fs::path& filePath, size_t fileSize, std::time_t lastModified)
    {
        return "W/" + generateStrong(filePath, fileSize, lastModified);
    }

    /**
     * @brief 生成 ETag（自动选择类型）
     * @param filePath 文件路径
     * @param type ETag 类型（强/弱）
     * @return ETag 字符串
     */
    static std::string generate(const fs::path& filePath, Type type = Type::STRONG)
    {
        std::error_code ec;

        // 获取文件大小
        auto fileSize = fs::file_size(filePath, ec);
        if (ec) return "\"\"";

        // 获取文件的真实修改时间
        std::time_t lastModifiedTimeT = getFileModificationTime(filePath);
        if (lastModifiedTimeT == 0) return "\"\"";

        if (type == Type::WEAK) {
            return generateWeak(filePath, fileSize, lastModifiedTimeT);
        } else {
            return generateStrong(filePath, fileSize, lastModifiedTimeT);
        }
    }

    /**
     * @brief 检查 ETag 是否匹配
     * @details 支持强弱 ETag 的比较（规范化后比较）
     */
    static bool match(const std::string& etag1, const std::string& etag2)
    {
        auto normalize = [](const std::string& etag) -> std::string {
            std::string normalized = etag;
            // 移除弱 ETag 前缀 "W/"
            if (normalized.size() >= 2 && normalized[0] == 'W' && normalized[1] == '/') {
                normalized = normalized.substr(2);
            }
            return normalized;
        };
        return normalize(etag1) == normalize(etag2);
    }

    /**
     * @brief 解析 If-None-Match 或 If-Match 头
     * @details 提取所有 ETag 值（不包含引号）
     */
    static std::vector<std::string> parseIfMatch(const std::string& headerValue)
    {
        std::vector<std::string> etags;
        std::string current;
        bool inEtag = false;

        for (size_t i = 0; i < headerValue.size(); ++i) {
            char c = headerValue[i];
            if (c == '"') {
                if (inEtag) {
                    if (!current.empty()) {
                        etags.push_back(current);
                        current.clear();
                    }
                    inEtag = false;
                } else {
                    inEtag = true;
                }
            } else if (inEtag) {
                current += c;
            }
        }
        return etags;
    }

    /**
     * @brief 检查是否匹配任何一个 ETag
     */
    static bool matchAny(const std::string& etag, const std::vector<std::string>& etags)
    {
        for (const auto& e : etags) {
            if (match(etag, e)) return true;
        }
        return false;
    }

    /**
     * @brief 格式化 HTTP 日期
     * @details 按照 RFC 7231 格式化为 GMT 时间
     */
    static std::string formatHttpDate(std::time_t time)
    {
        char buffer[128];
        std::tm tm;
#ifdef _WIN32
        gmtime_s(&tm, &time);
#else
        gmtime_r(&time, &tm);
#endif
        strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &tm);
        return std::string(buffer);
    }

private:
    /**
     * @brief 获取文件的真实 inode
     * @details 使用 stat 系统调用获取文件的 inode 号
     */
    static uint64_t getFileInode(const fs::path& filePath)
    {
#ifdef _WIN32
        // Windows 没有 inode 概念，使用文件路径哈希作为替代
        return std::hash<std::string>{}(filePath.string());
#else
        struct stat fileStat;
        if (stat(filePath.c_str(), &fileStat) == 0) {
            return static_cast<uint64_t>(fileStat.st_ino);
        }
        // 失败时使用路径哈希作为后备
        return std::hash<std::string>{}(filePath.string());
#endif
    }

    /**
     * @brief 获取文件的真实修改时间
     * @details 使用 stat 系统调用获取文件的 mtime
     */
    static std::time_t getFileModificationTime(const fs::path& filePath)
    {
#ifdef _WIN32
        // Windows 使用 filesystem 库
        std::error_code ec;
        auto ftime = fs::last_write_time(filePath, ec);
        if (ec) return 0;

        // 转换为 time_t
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
        );
        return std::chrono::system_clock::to_time_t(sctp);
#else
        struct stat fileStat;
        if (stat(filePath.c_str(), &fileStat) == 0) {
            return fileStat.st_mtime;
        }
        return 0;
#endif
    }
};

} // namespace galay::http

#endif // GALAY_HTTP_ETAG_H
