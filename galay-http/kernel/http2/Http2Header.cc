#include "Http2Header.h"
#include <charconv>
#include <cctype>

namespace galay::http
{
    Http2Header::Http2Header(const std::vector<HpackHeaderField>& fields)
    {
        for (const auto& field : fields) {
            set(field.name, field.value);
        }
    }

    Http2Header::Http2Header(const std::map<std::string, std::string>& headers)
    {
        for (const auto& [name, value] : headers) {
            set(name, value);
        }
    }

    Http2Header& Http2Header::setMethod(const std::string& method)
    {
        return set(":method", method);
    }

    Http2Header& Http2Header::setPath(const std::string& path)
    {
        return set(":path", path);
    }

    Http2Header& Http2Header::setScheme(const std::string& scheme)
    {
        return set(":scheme", scheme);
    }

    Http2Header& Http2Header::setAuthority(const std::string& authority)
    {
        return set(":authority", authority);
    }

    Http2Header& Http2Header::setStatus(int status_code)
    {
        return set(":status", std::to_string(status_code));
    }

    Http2Header& Http2Header::setStatus(HttpStatusCode status_code)
    {
        return setStatus(static_cast<int>(status_code));
    }

    int Http2Header::status() const
    {
        const std::string value = get(":status");
        if (value.empty()) {
            return 0;
        }
        int result = 0;
        auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
        if (ec != std::errc()) {
            return 0;
        }
        return result;
    }

    Http2Header& Http2Header::set(const std::string& name, const std::string& value)
    {
        m_headers[normalizeName(name)] = value;
        return *this;
    }

    Http2Header& Http2Header::add(const std::string& name, const std::string& value)
    {
        std::string key = normalizeName(name);
        auto it = m_headers.find(key);
        if (it == m_headers.end()) {
            m_headers[key] = value;
        } else {
            it->second.append(", ").append(value);
        }
        return *this;
    }

    std::string Http2Header::get(const std::string& name) const
    {
        auto it = m_headers.find(normalizeName(name));
        if (it == m_headers.end()) {
            return {};
        }
        return it->second;
    }

    bool Http2Header::contains(const std::string& name) const
    {
        return m_headers.find(normalizeName(name)) != m_headers.end();
    }

    Http2Header& Http2Header::setContentType(const std::string& type)
    {
        return set("content-type", type);
    }

    Http2Header& Http2Header::setContentLength(size_t length)
    {
        return set("content-length", std::to_string(length));
    }

    size_t Http2Header::contentLength() const
    {
        const std::string value = get("content-length");
        if (value.empty()) {
            return 0;
        }
        uint64_t parsed = 0;
        auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (ec != std::errc()) {
            return 0;
        }
        return static_cast<size_t>(parsed);
    }

    std::vector<HpackHeaderField> Http2Header::toHeaderFields() const
    {
        std::vector<HpackHeaderField> fields;
        fields.reserve(m_headers.size());

        // 伪头部必须排在普通头部之前
        static const std::vector<std::string> pseudo_order = {
            ":method",
            ":scheme",
            ":authority",
            ":path",
            ":status"
        };

        for (const auto& pseudo : pseudo_order) {
            auto it = m_headers.find(pseudo);
            if (it != m_headers.end()) {
                fields.push_back({it->first, it->second});
            }
        }

        // 普通头部保持字典序
        for (const auto& [name, value] : m_headers) {
            if (isPseudoHeader(name)) {
                continue;
            }
            fields.push_back({name, value});
        }

        return fields;
    }

    std::string Http2Header::encode() const
    {
        HpackEncoder encoder;
        return encoder.encodeHeaders(toHeaderFields());
    }

    std::string Http2Header::toString() const
    {
        std::ostringstream oss;
        for (const auto& [name, value] : m_headers) {
            oss << name << ": " << value << '\n';
        }
        return oss.str();
    }

    std::string Http2Header::normalizeName(const std::string& name)
    {
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return lower;
    }

    bool Http2Header::isPseudoHeader(const std::string& name)
    {
        return !name.empty() && name.front() == ':';
    }
}
