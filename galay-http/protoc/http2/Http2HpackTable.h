#ifndef GALAY_HTTP2_HPACK_TABLE_H
#define GALAY_HTTP2_HPACK_TABLE_H

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <cstdint>

namespace galay::http
{
    /**
     * @brief HPACK 头部字段
     */
    struct HpackHeaderField
    {
        std::string name;
        std::string value;
        
        HpackHeaderField() = default;
        HpackHeaderField(const std::string& n, const std::string& v)
            : name(n), value(v) {}
        
        size_t size() const {
            // RFC 7541: 条目大小 = name长度 + value长度 + 32
            return name.size() + value.size() + 32;
        }
    };
    
    /**
     * @brief HPACK 静态表
     * 
     * RFC 7541 附录 A 定义的静态表（61 个条目）
     */
    class HpackStaticTable
    {
    public:
        /**
         * @brief 获取静态表条目
         * @param index 索引（1-61）
         * @return 头部字段，如果索引无效返回 nullopt
         */
        static std::optional<HpackHeaderField> get(size_t index);
        
        /**
         * @brief 查找完全匹配的条目（名称和值都匹配）
         * @param name 头部名称
         * @param value 头部值
         * @return 索引，如果未找到返回 0
         */
        static size_t findExactMatch(const std::string& name, const std::string& value);
        
        /**
         * @brief 查找名称匹配的条目
         * @param name 头部名称
         * @return 索引，如果未找到返回 0
         */
        static size_t findNameMatch(const std::string& name);
        
        /**
         * @brief 获取静态表大小
         */
        static constexpr size_t size() { return 61; }
        
    private:
        // RFC 7541 附录 A - 静态表
        static const HpackHeaderField STATIC_TABLE[61];
    };
    
    /**
     * @brief HPACK 动态表
     * 
     * 使用 FIFO 策略，超过最大大小时驱逐最旧的条目
     */
    class HpackDynamicTable
    {
    public:
        /**
         * @brief 构造函数
         * @param max_size 最大大小（字节），默认 4096
         */
        explicit HpackDynamicTable(size_t max_size = 4096);
        
        /**
         * @brief 添加条目到动态表头部
         */
        void add(const std::string& name, const std::string& value);
        
        /**
         * @brief 获取动态表条目
         * @param index 索引（从 1 开始）
         * @return 头部字段，如果索引无效返回 nullopt
         */
        std::optional<HpackHeaderField> get(size_t index) const;
        
        /**
         * @brief 查找完全匹配的条目
         * @return 索引（相对于动态表，从 1 开始），如果未找到返回 0
         */
        size_t findExactMatch(const std::string& name, const std::string& value) const;
        
        /**
         * @brief 查找名称匹配的条目
         * @return 索引（相对于动态表，从 1 开始），如果未找到返回 0
         */
        size_t findNameMatch(const std::string& name) const;
        
        /**
         * @brief 设置最大大小
         */
        void setMaxSize(size_t max_size);
        
        /**
         * @brief 获取当前大小
         */
        size_t currentSize() const { return m_current_size; }
        
        /**
         * @brief 获取最大大小
         */
        size_t maxSize() const { return m_max_size; }
        
        /**
         * @brief 获取条目数量
         */
        size_t size() const { return m_entries.size(); }
        
        /**
         * @brief 清空动态表
         */
        void clear();
        
    private:
        void evict();  // 驱逐最旧的条目直到满足大小限制
        
        std::vector<HpackHeaderField> m_entries;  // 动态表条目（索引 0 是最新的）
        size_t m_max_size;                        // 最大大小（字节）
        size_t m_current_size;                    // 当前大小（字节）
    };
    
    /**
     * @brief HPACK 索引表（组合静态表和动态表）
     */
    class HpackTable
    {
    public:
        /**
         * @brief 构造函数
         * @param max_dynamic_size 动态表最大大小，默认 4096
         */
        explicit HpackTable(size_t max_dynamic_size = 4096);
        
        /**
         * @brief 获取表条目
         * @param index 索引（从 1 开始，1-61 是静态表，62+ 是动态表）
         * @return 头部字段，如果索引无效返回 nullopt
         */
        std::optional<HpackHeaderField> get(size_t index) const;
        
        /**
         * @brief 添加条目到动态表
         */
        void add(const std::string& name, const std::string& value);
        
        /**
         * @brief 查找完全匹配的条目
         * @return 索引（从 1 开始），如果未找到返回 0
         */
        size_t findExactMatch(const std::string& name, const std::string& value) const;
        
        /**
         * @brief 查找名称匹配的条目
         * @return 索引（从 1 开始），如果未找到返回 0
         */
        size_t findNameMatch(const std::string& name) const;
        
        /**
         * @brief 设置动态表最大大小
         */
        void setDynamicTableMaxSize(size_t max_size);
        
        /**
         * @brief 获取动态表当前大小
         */
        size_t dynamicTableSize() const;
        
    private:
        HpackDynamicTable m_dynamic_table;
    };
}

#endif // GALAY_HTTP2_HPACK_TABLE_H

