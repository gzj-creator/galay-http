#include "Http2HpackTable.h"
#include <algorithm>

namespace galay::http
{
    // RFC 7541 附录 A - 静态表
    const HpackHeaderField HpackStaticTable::STATIC_TABLE[61] = {
        {":authority", ""},                      // 1
        {":method", "GET"},                      // 2
        {":method", "POST"},                     // 3
        {":path", "/"},                          // 4
        {":path", "/index.html"},                // 5
        {":scheme", "http"},                     // 6
        {":scheme", "https"},                    // 7
        {":status", "200"},                      // 8
        {":status", "204"},                      // 9
        {":status", "206"},                      // 10
        {":status", "304"},                      // 11
        {":status", "400"},                      // 12
        {":status", "404"},                      // 13
        {":status", "500"},                      // 14
        {"accept-charset", ""},                  // 15
        {"accept-encoding", "gzip, deflate"},    // 16
        {"accept-language", ""},                 // 17
        {"accept-ranges", ""},                   // 18
        {"accept", ""},                          // 19
        {"access-control-allow-origin", ""},     // 20
        {"age", ""},                             // 21
        {"allow", ""},                           // 22
        {"authorization", ""},                   // 23
        {"cache-control", ""},                   // 24
        {"content-disposition", ""},             // 25
        {"content-encoding", ""},                // 26
        {"content-language", ""},                // 27
        {"content-length", ""},                  // 28
        {"content-location", ""},                // 29
        {"content-range", ""},                   // 30
        {"content-type", ""},                    // 31
        {"cookie", ""},                          // 32
        {"date", ""},                            // 33
        {"etag", ""},                            // 34
        {"expect", ""},                          // 35
        {"expires", ""},                         // 36
        {"from", ""},                            // 37
        {"host", ""},                            // 38
        {"if-match", ""},                        // 39
        {"if-modified-since", ""},               // 40
        {"if-none-match", ""},                   // 41
        {"if-range", ""},                        // 42
        {"if-unmodified-since", ""},             // 43
        {"last-modified", ""},                   // 44
        {"link", ""},                            // 45
        {"location", ""},                        // 46
        {"max-forwards", ""},                    // 47
        {"proxy-authenticate", ""},              // 48
        {"proxy-authorization", ""},             // 49
        {"range", ""},                           // 50
        {"referer", ""},                         // 51
        {"refresh", ""},                         // 52
        {"retry-after", ""},                     // 53
        {"server", ""},                          // 54
        {"set-cookie", ""},                      // 55
        {"strict-transport-security", ""},       // 56
        {"transfer-encoding", ""},               // 57
        {"user-agent", ""},                      // 58
        {"vary", ""},                            // 59
        {"via", ""},                             // 60
        {"www-authenticate", ""}                 // 61
    };
    
    std::optional<HpackHeaderField> HpackStaticTable::get(size_t index)
    {
        if (index == 0 || index > 61) {
            return std::nullopt;
        }
        return STATIC_TABLE[index - 1];
    }
    
    size_t HpackStaticTable::findExactMatch(const std::string& name, const std::string& value)
    {
        for (size_t i = 0; i < 61; ++i) {
            if (STATIC_TABLE[i].name == name && STATIC_TABLE[i].value == value) {
                return i + 1;
            }
        }
        return 0;
    }
    
    size_t HpackStaticTable::findNameMatch(const std::string& name)
    {
        for (size_t i = 0; i < 61; ++i) {
            if (STATIC_TABLE[i].name == name) {
                return i + 1;
            }
        }
        return 0;
    }
    
    // ==================== 动态表 ====================
    
    HpackDynamicTable::HpackDynamicTable(size_t max_size)
        : m_max_size(max_size), m_current_size(0)
    {
    }
    
    void HpackDynamicTable::add(const std::string& name, const std::string& value)
    {
        HpackHeaderField entry(name, value);
        size_t entry_size = entry.size();
        
        // 如果条目本身就超过最大大小，清空动态表
        if (entry_size > m_max_size) {
            clear();
            return;
        }
        
        // 驱逐旧条目以腾出空间
        while (m_current_size + entry_size > m_max_size && !m_entries.empty()) {
            m_current_size -= m_entries.back().size();
            m_entries.pop_back();
        }
        
        // 添加新条目到表头
        m_entries.insert(m_entries.begin(), entry);
        m_current_size += entry_size;
    }
    
    std::optional<HpackHeaderField> HpackDynamicTable::get(size_t index) const
    {
        if (index == 0 || index > m_entries.size()) {
            return std::nullopt;
        }
        return m_entries[index - 1];
    }
    
    size_t HpackDynamicTable::findExactMatch(const std::string& name, const std::string& value) const
    {
        for (size_t i = 0; i < m_entries.size(); ++i) {
            if (m_entries[i].name == name && m_entries[i].value == value) {
                return i + 1;
            }
        }
        return 0;
    }
    
    size_t HpackDynamicTable::findNameMatch(const std::string& name) const
    {
        for (size_t i = 0; i < m_entries.size(); ++i) {
            if (m_entries[i].name == name) {
                return i + 1;
            }
        }
        return 0;
    }
    
    void HpackDynamicTable::setMaxSize(size_t max_size)
    {
        m_max_size = max_size;
        evict();
    }
    
    void HpackDynamicTable::clear()
    {
        m_entries.clear();
        m_current_size = 0;
    }
    
    void HpackDynamicTable::evict()
    {
        while (m_current_size > m_max_size && !m_entries.empty()) {
            m_current_size -= m_entries.back().size();
            m_entries.pop_back();
        }
    }
    
    // ==================== 组合表 ====================
    
    HpackTable::HpackTable(size_t max_dynamic_size)
        : m_dynamic_table(max_dynamic_size)
    {
    }
    
    std::optional<HpackHeaderField> HpackTable::get(size_t index) const
    {
        if (index == 0) {
            return std::nullopt;
        }
        
        // 1-61: 静态表
        if (index <= 61) {
            return HpackStaticTable::get(index);
        }
        
        // 62+: 动态表
        return m_dynamic_table.get(index - 61);
    }
    
    void HpackTable::add(const std::string& name, const std::string& value)
    {
        m_dynamic_table.add(name, value);
    }
    
    size_t HpackTable::findExactMatch(const std::string& name, const std::string& value) const
    {
        // 先查找动态表（优先使用最近的条目）
        size_t dynamic_index = m_dynamic_table.findExactMatch(name, value);
        if (dynamic_index != 0) {
            return 61 + dynamic_index;
        }
        
        // 再查找静态表
        return HpackStaticTable::findExactMatch(name, value);
    }
    
    size_t HpackTable::findNameMatch(const std::string& name) const
    {
        // 先查找动态表
        size_t dynamic_index = m_dynamic_table.findNameMatch(name);
        if (dynamic_index != 0) {
            return 61 + dynamic_index;
        }
        
        // 再查找静态表
        return HpackStaticTable::findNameMatch(name);
    }
    
    void HpackTable::setDynamicTableMaxSize(size_t max_size)
    {
        m_dynamic_table.setMaxSize(max_size);
    }
    
    size_t HpackTable::dynamicTableSize() const
    {
        return m_dynamic_table.currentSize();
    }
}

