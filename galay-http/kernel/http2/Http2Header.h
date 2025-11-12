#ifndef GALAY_HTTP2_HEADER_H
#define GALAY_HTTP2_HEADER_H

#include <map>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include "galay-http/protoc/http/HttpBase.h"
#include "galay-http/protoc/http2/Http2Error.h"
#include "galay-http/protoc/http2/Http2Hpack.h"

namespace galay::http
{
    /**
     * @brief HTTP/2 头部封装
     *
     * 提供常用头部构建方法，并负责 HPACK 序列化。
     * 不涉及网络收发逻辑。
     */
    class Http2Header
    {
    public:
        Http2Header() = default;
        explicit Http2Header(const std::vector<HpackHeaderField>& fields);
        explicit Http2Header(const std::map<std::string, std::string>& headers);

        // ========== 请求伪头部 ==========
        Http2Header& setMethod(const std::string& method);
        Http2Header& setPath(const std::string& path);
        Http2Header& setScheme(const std::string& scheme);
        Http2Header& setAuthority(const std::string& authority);

        std::string method() const { return get(":method"); }
        std::string path() const { return get(":path"); }
        std::string scheme() const { return get(":scheme"); }
        std::string authority() const { return get(":authority"); }

        // ========== 响应伪头部 ==========
        Http2Header& setStatus(int status_code);
        Http2Header& setStatus(HttpStatusCode status_code);
        int status() const;

        // ========== 通用头部操作 ==========
        Http2Header& set(const std::string& name, const std::string& value);
        Http2Header& add(const std::string& name, const std::string& value);

        std::string get(const std::string& name) const;
        bool contains(const std::string& name) const;
        const std::map<std::string, std::string>& all() const { return m_headers; }

        Http2Header& setContentType(const std::string& type);
        Http2Header& setContentLength(size_t length);
        std::string contentType() const { return get("content-type"); }
        size_t contentLength() const;

        // ========== 序列化 ==========
        std::vector<HpackHeaderField> toHeaderFields() const;
        std::string encode() const;
        std::string toString() const;

    private:
        static std::string normalizeName(const std::string& name);
        static bool isPseudoHeader(const std::string& name);

        std::map<std::string, std::string> m_headers;
    };
}

#endif // GALAY_HTTP2_HEADER_H
