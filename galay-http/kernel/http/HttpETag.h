#ifndef GALAY_HTTP_ETAG_H
#define GALAY_HTTP_ETAG_H

#include <string>
#include <ctime>
#include <cstdint>
#include <filesystem>

namespace galay::http
{

namespace fs = std::filesystem;

/**
 * @brief ETag 生成器
 * @details 为文件生成 ETag，支持强 ETag 和弱 ETag
 */
class ETagGenerator
{
public:
    /**
     * @brief ETag 类型
     */
    enum class Type
    {
        STRONG,  // 强 ETag
        WEAK     // 弱 ETag
    };

    /**
     * @brief 生成文件的强 ETag
     */
    static std::string generateStrong(const fs::path& filePath, size_t fileSize, std::time_t lastModified)
    {
        uint64_t inode = getFileInode(filePath);
        char etag[128];
        snprintf(etag, sizeof(etag), "\"%lu-%zu-%ld\"",
                 static_cast<unsigned long>(inode), fileSize, lastModified);
        return std::string(etag);
    }

    /**
     * @brief 生成文件的弱 ETag
     */
    static std::string generateWeak(const fs::path& filePath, size_t fileSize, std::time_t lastModified)
    {
        return "W/" + generateStrong(filePath, fileSize, lastModified);
    }

    /**
     * @brief 生成 ETag（自动选择类型）
     */
    static std::string generate(const fs::path& filePath, Type type = Type::STRONG)
    {
        std::error_code ec;
        auto fileSize = fs::file_size(filePath, ec);
        if (ec) return "\"\"";

        // 简化：使用当前时间作为修改时间
        std::time_t lastModifiedTimeT = std::time(nullptr);

        if (type == Type::WEAK) {
            return generateWeak(filePath, fileSize, lastModifiedTimeT);
        } else {
            return generateStrong(filePath, fileSize, lastModifiedTimeT);
        }
    }

    /**
     * @brief 检查 ETag 是否匹配
     */
    static bool match(const std::string& etag1, const std::string& etag2)
    {
        auto normalize = [](const std::string& etag) -> std::string {
            std::string normalized = etag;
            if (normalized.size() >= 2 && normalized[0] == 'W' && normalized[1] == '/') {
                normalized = normalized.substr(2);
            }
            return normalized;
        };
        return normalize(etag1) == normalize(etag2);
    }

    /**
     * @brief 解析 If-None-Match 或 If-Match 头
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
    static uint64_t getFileInode(const fs::path& filePath)
    {
        return std::hash<std::string>{}(filePath.string());
    }
};

} // namespace galay::http

#endif // GALAY_HTTP_ETAG_H
