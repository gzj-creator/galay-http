#ifndef GALAY_PATH_SECURITY_H
#define GALAY_PATH_SECURITY_H

#include <filesystem>
#include <string>
#include <vector>
#include <set>

namespace galay::http
{

namespace fs = std::filesystem;

/**
 * @brief 路径安全检查工具类
 * @details 提供路径遍历攻击防护、黑名单检查、符号链接验证等功能
 */
class PathSecurity
{
public:
    /**
     * @brief 构造函数
     * @param baseDir 基础目录（所有文件必须在此目录下）
     */
    explicit PathSecurity(const fs::path& baseDir)
        : m_baseDir(fs::canonical(baseDir))
    {
        initializeBlacklist();
    }

    /**
     * @brief 检查路径是否安全
     * @param path 要检查的路径
     * @param error 错误信息（输出参数）
     * @return 是否安全
     */
    bool isPathSafe(const fs::path& path, std::string& error) const
    {
        try {
            // 1. 检查路径是否存在
            if (!fs::exists(path)) {
                error = "Path does not exist";
                return false;
            }

            // 2. 获取规范路径
            fs::path canonicalPath = fs::canonical(path);

            // 3. 检查是否在基础目录内
            if (!isUnderBaseDirectory(canonicalPath)) {
                error = "Path is outside base directory (path traversal attempt)";
                return false;
            }

            // 4. 检查符号链接
            if (fs::is_symlink(path)) {
                if (!isSymlinkSafe(path, error)) {
                    return false;
                }
            }

            // 5. 检查黑名单
            if (isBlacklisted(canonicalPath)) {
                error = "Path matches blacklist pattern";
                return false;
            }

            // 6. 检查隐藏文件（可选）
            if (m_blockHiddenFiles && isHiddenFile(canonicalPath)) {
                error = "Access to hidden files is not allowed";
                return false;
            }

            return true;

        } catch (const fs::filesystem_error& e) {
            error = std::string("Filesystem error: ") + e.what();
            return false;
        }
    }

    /**
     * @brief 设置是否阻止访问隐藏文件
     * @param block 是否阻止
     */
    void setBlockHiddenFiles(bool block)
    {
        m_blockHiddenFiles = block;
    }

    /**
     * @brief 添加黑名单模式
     * @param pattern 文件名或扩展名模式
     */
    void addBlacklistPattern(const std::string& pattern)
    {
        m_blacklist.insert(pattern);
    }

    /**
     * @brief 移除黑名单模式
     * @param pattern 文件名或扩展名模式
     */
    void removeBlacklistPattern(const std::string& pattern)
    {
        m_blacklist.erase(pattern);
    }

    /**
     * @brief 清空黑名单
     */
    void clearBlacklist()
    {
        m_blacklist.clear();
    }

    /**
     * @brief 获取基础目录
     * @return 基础目录路径
     */
    const fs::path& getBaseDirectory() const
    {
        return m_baseDir;
    }

private:
    /**
     * @brief 初始化默认黑名单
     */
    void initializeBlacklist()
    {
        // 版本控制系统
        m_blacklist.insert(".git");
        m_blacklist.insert(".svn");
        m_blacklist.insert(".hg");
        m_blacklist.insert(".bzr");

        // 配置文件
        m_blacklist.insert(".env");
        m_blacklist.insert(".env.local");
        m_blacklist.insert(".env.production");
        m_blacklist.insert("config.json");
        m_blacklist.insert("config.yml");
        m_blacklist.insert("config.yaml");

        // Web 服务器配置
        m_blacklist.insert(".htaccess");
        m_blacklist.insert(".htpasswd");
        m_blacklist.insert("web.config");
        m_blacklist.insert("nginx.conf");
        m_blacklist.insert("apache.conf");

        // 敏感文件
        m_blacklist.insert("id_rsa");
        m_blacklist.insert("id_dsa");
        m_blacklist.insert("id_ecdsa");
        m_blacklist.insert("id_ed25519");
        m_blacklist.insert(".ssh");
        m_blacklist.insert("authorized_keys");
        m_blacklist.insert("known_hosts");

        // 数据库文件
        m_blacklist.insert(".db");
        m_blacklist.insert(".sqlite");
        m_blacklist.insert(".sqlite3");

        // 备份文件
        m_blacklist.insert(".bak");
        m_blacklist.insert(".backup");
        m_blacklist.insert(".old");
        m_blacklist.insert(".orig");
        m_blacklist.insert(".swp");
        m_blacklist.insert("~");

        // IDE 配置
        m_blacklist.insert(".vscode");
        m_blacklist.insert(".idea");
        m_blacklist.insert(".DS_Store");
    }

    /**
     * @brief 检查路径是否在基础目录内
     * @param path 规范路径
     * @return 是否在基础目录内
     */
    bool isUnderBaseDirectory(const fs::path& path) const
    {
        // 使用 mismatch 检查路径前缀
        auto [baseIt, pathIt] = std::mismatch(
            m_baseDir.begin(), m_baseDir.end(),
            path.begin(), path.end()
        );

        // 如果基础目录的所有部分都匹配，则路径在基础目录内
        return baseIt == m_baseDir.end();
    }

    /**
     * @brief 检查符号链接是否安全
     * @param symlinkPath 符号链接路径
     * @param error 错误信息（输出参数）
     * @return 是否安全
     */
    bool isSymlinkSafe(const fs::path& symlinkPath, std::string& error) const
    {
        try {
            // 读取符号链接目标
            fs::path target = fs::read_symlink(symlinkPath);

            // 如果是相对路径，转换为绝对路径
            if (target.is_relative()) {
                target = symlinkPath.parent_path() / target;
            }

            // 获取目标的规范路径
            fs::path canonicalTarget = fs::canonical(target);

            // 检查目标是否在基础目录内
            if (!isUnderBaseDirectory(canonicalTarget)) {
                error = "Symlink target is outside base directory";
                return false;
            }

            return true;

        } catch (const fs::filesystem_error& e) {
            error = std::string("Failed to resolve symlink: ") + e.what();
            return false;
        }
    }

    /**
     * @brief 检查路径是否匹配黑名单
     * @param path 路径
     * @return 是否匹配黑名单
     */
    bool isBlacklisted(const fs::path& path) const
    {
        // 检查路径中的每个部分
        for (const auto& part : path) {
            std::string partStr = part.string();

            // 检查完整文件名
            if (m_blacklist.count(partStr) > 0) {
                return true;
            }

            // 检查扩展名
            if (path.has_extension()) {
                std::string ext = path.extension().string();
                if (m_blacklist.count(ext) > 0) {
                    return true;
                }
            }
        }

        return false;
    }

    /**
     * @brief 检查是否为隐藏文件
     * @param path 路径
     * @return 是否为隐藏文件
     */
    bool isHiddenFile(const fs::path& path) const
    {
        std::string filename = path.filename().string();
        return !filename.empty() && filename[0] == '.';
    }

private:
    fs::path m_baseDir;                    ///< 基础目录
    std::set<std::string> m_blacklist;     ///< 黑名单
    bool m_blockHiddenFiles = true;        ///< 是否阻止隐藏文件
};

} // namespace galay::http

#endif // GALAY_PATH_SECURITY_H
