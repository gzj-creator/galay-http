#ifndef GALAY_FILE_DESCRIPTOR_H
#define GALAY_FILE_DESCRIPTOR_H

#include <fcntl.h>
#include <unistd.h>
#include <system_error>
#include <string>

namespace galay::http
{

/**
 * @brief RAII 文件描述符管理类
 * @details 自动管理文件描述符的生命周期，防止资源泄漏
 */
class FileDescriptor
{
public:
    /**
     * @brief 默认构造函数
     */
    FileDescriptor() noexcept
        : m_fd(-1)
    {
    }

    /**
     * @brief 构造函数并打开文件
     * @param path 文件路径
     * @param flags 打开标志（O_RDONLY, O_WRONLY, O_RDWR 等）
     * @throws std::system_error 打开失败时抛出异常
     */
    FileDescriptor(const char* path, int flags)
        : m_fd(-1)
    {
        open(path, flags);
    }

    /**
     * @brief 构造函数并打开文件（带权限）
     * @param path 文件路径
     * @param flags 打开标志
     * @param mode 文件权限（创建新文件时使用）
     * @throws std::system_error 打开失败时抛出异常
     */
    FileDescriptor(const char* path, int flags, mode_t mode)
        : m_fd(-1)
    {
        open(path, flags, mode);
    }

    /**
     * @brief 析构函数 - 自动关闭文件描述符
     */
    ~FileDescriptor() noexcept
    {
        close();
    }

    // 禁用拷贝
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    // 启用移动
    FileDescriptor(FileDescriptor&& other) noexcept
        : m_fd(other.m_fd)
    {
        other.m_fd = -1;
    }

    FileDescriptor& operator=(FileDescriptor&& other) noexcept
    {
        if (this != &other) {
            close();
            m_fd = other.m_fd;
            other.m_fd = -1;
        }
        return *this;
    }

    /**
     * @brief 打开文件
     * @param path 文件路径
     * @param flags 打开标志
     * @throws std::system_error 打开失败时抛出异常
     */
    void open(const char* path, int flags)
    {
        close();
        m_fd = ::open(path, flags);
        if (m_fd < 0) {
            throw std::system_error(errno, std::generic_category(), "Failed to open file");
        }
    }

    /**
     * @brief 打开文件（带权限）
     * @param path 文件路径
     * @param flags 打开标志
     * @param mode 文件权限
     * @throws std::system_error 打开失败时抛出异常
     */
    void open(const char* path, int flags, mode_t mode)
    {
        close();
        m_fd = ::open(path, flags, mode);
        if (m_fd < 0) {
            throw std::system_error(errno, std::generic_category(), "Failed to open file");
        }
    }

    /**
     * @brief 关闭文件描述符
     */
    void close() noexcept
    {
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
    }

    /**
     * @brief 获取文件描述符
     * @return 文件描述符
     */
    int get() const noexcept
    {
        return m_fd;
    }

    /**
     * @brief 检查文件描述符是否有效
     * @return 是否有效
     */
    bool valid() const noexcept
    {
        return m_fd >= 0;
    }

    /**
     * @brief 释放文件描述符所有权（不关闭）
     * @return 文件描述符
     */
    int release() noexcept
    {
        int fd = m_fd;
        m_fd = -1;
        return fd;
    }

    /**
     * @brief 交换两个文件描述符
     * @param other 另一个文件描述符对象
     */
    void swap(FileDescriptor& other) noexcept
    {
        std::swap(m_fd, other.m_fd);
    }

    /**
     * @brief bool 转换操作符
     */
    explicit operator bool() const noexcept
    {
        return valid();
    }

    /**
     * @brief 获取文件描述符（用于函数调用）
     */
    int operator*() const noexcept
    {
        return m_fd;
    }

private:
    int m_fd;
};

} // namespace galay::http

#endif // GALAY_FILE_DESCRIPTOR_H
