#include "HttpHeader.h"
#include <cassert>

namespace galay::http 
{
    HeaderPair::HeaderPair()
    {
    }

    HeaderPair::HeaderPair(const HeaderPair &other)
    {
        if(this != &other)
        {
            m_headerPairs = other.m_headerPairs;
        }
    }

    HeaderPair::HeaderPair(HeaderPair &&other)
    {
        if(this != &other)
        {
            std::swap(m_headerPairs, other.m_headerPairs);
        }
    }
    bool HeaderPair::hasKey(const std::string &key) const
    {
        return m_headerPairs.contains(key);
    }

    std::string HeaderPair::getValue(const std::string& key) const
    {
        auto it = m_headerPairs.find(key);
        if (it != m_headerPairs.end())
            return it->second;
        return "";
    }

    HttpErrorCode HeaderPair::removeHeaderPair(const std::string& key)
    {
        if (m_headerPairs.contains(key))
        {
            m_headerPairs.erase(key);
        }
        else
        {
            return kHeaderPairNotExist;
        }
        return kNoError;
    }

    HttpErrorCode HeaderPair::addHeaderPairIfNotExist(const std::string& key, const std::string& value)
    {
        if (m_headerPairs.contains(key))
        {
            return kHeaderPairExist;
        }
        this->m_headerPairs.emplace(key, value);
        return kNoError;
    }

    HttpErrorCode HeaderPair::addHeaderPair(const std::string& key, const std::string& value)
    {
        this->m_headerPairs[key] = value;
        return kNoError;
    }

    std::string HeaderPair::toString() const
    {
        if (m_headerPairs.empty()) {
            return "";
        }
        
        // 预估算字符串大小，减少重分配
        size_t estimated_size = 0;
        for (const auto& [k, v] : m_headerPairs) {
            estimated_size += k.size() + v.size() + 4; // "key: value\r\n"
        }
        
        std::string result;
        result.reserve(estimated_size);
        
        // 直接拼接，避免 ostringstream 开销
        for (const auto& [k, v] : m_headerPairs) {
            result += k;
            result += ": ";
            result += v;
            result += "\r\n";
        }
        return result;
    }

    void HeaderPair::clear()
    {
        if(!m_headerPairs.empty()) m_headerPairs.clear();
    }

    HeaderPair &HeaderPair::operator=(const HeaderPair &other)
    {
        m_headerPairs = other.m_headerPairs;
        return *this;
    }

    HeaderPair &HeaderPair::operator=(HeaderPair &&other)
    {
        std::swap(m_headerPairs, other.m_headerPairs);
        return *this;
    }

    HttpMethod& HttpRequestHeader::method()
    {
        return this->m_method;
    }

    std::string& HttpRequestHeader::uri()
    {
        return this->m_uri;
    }

    HttpVersion& HttpRequestHeader::version()
    {
        return this->m_version;
    }

    std::map<std::string,std::string>& HttpRequestHeader::args()
    {
        return this->m_argList;
    }


    HeaderPair& HttpRequestHeader::headerPairs()
    {
        return this->m_headerPairs;
    }

    HttpErrorCode HttpRequestHeader::parseChar(char c)
    {
        switch (m_parseState) {
        case RequestParseState::Method:
            if (c == ' ') {
                m_method = stringToHttpMethod(m_parseMethodStr);
                m_parseState = RequestParseState::MethodSP;
            } else if (c == '\r' || c == '\n') {
                return kBadRequest;
            } else {
                m_parseMethodStr += c;
            }
            break;

        case RequestParseState::MethodSP:
            if (c == ' ') {
                // skip extra spaces
            } else if (c == '\r' || c == '\n') {
                return kBadRequest;
            } else {
                m_parseUriStr += c;
                m_parseState = RequestParseState::Uri;
            }
            break;

        case RequestParseState::Uri:
            if (c == ' ') {
                std::string full_uri = convertFromUri(m_parseUriStr, false);
                parseArgs(full_uri);
                if (m_uri.empty()) {
                    m_uri = full_uri;
                }
                m_parseState = RequestParseState::UriSP;
            } else if (c == '\r' || c == '\n') {
                return kBadRequest;
            } else {
                m_parseUriStr += c;
            }
            break;

        case RequestParseState::UriSP:
            if (c == ' ') {
                // skip extra spaces
            } else if (c == '\r' || c == '\n') {
                return kBadRequest;
            } else {
                m_parseVersionStr += c;
                m_parseState = RequestParseState::Version;
            }
            break;

        case RequestParseState::Version:
            if (c == '\r') {
                m_version = stringToHttpVersion(m_parseVersionStr);
                if (m_version == HttpVersion::HttpVersion_Unknown) {
                    return kVersionNotSupport;
                }
                // 只支持 HTTP/1.0 和 HTTP/1.1
                if (m_version != HttpVersion::HttpVersion_1_0 && m_version != HttpVersion::HttpVersion_1_1) {
                    return kVersionNotSupport;
                }
                m_parseState = RequestParseState::VersionCR;
            } else if (c == '\n') {
                return kBadRequest;
            } else {
                m_parseVersionStr += c;
            }
            break;

        case RequestParseState::VersionCR:
            if (c == '\n') {
                m_parseState = RequestParseState::VersionLF;
            } else {
                return kBadRequest;
            }
            break;

        case RequestParseState::VersionLF:
            if (c == '\r') {
                m_parseState = RequestParseState::HeaderEndCR;
            } else if (c == '\n') {
                return kBadRequest;
            } else {
                m_parseHeaderKey += c;
                m_parseState = RequestParseState::HeaderKey;
            }
            break;

        case RequestParseState::HeaderKey:
            if (c == ':') {
                m_parseState = RequestParseState::HeaderColon;
            } else if (c == '\r' || c == '\n') {
                return kBadRequest;
            } else {
                m_parseHeaderKey += c;
            }
            break;

        case RequestParseState::HeaderColon:
            if (c == ' ') {
                m_parseState = RequestParseState::HeaderSpace;
            } else if (c == '\r') {
                m_headerPairs.addHeaderPair(m_parseHeaderKey, m_parseHeaderValue);
                m_parseHeaderKey.clear();
                m_parseHeaderValue.clear();
                m_parseState = RequestParseState::HeaderCR;
            } else {
                m_parseHeaderValue += c;
                m_parseState = RequestParseState::HeaderValue;
            }
            break;

        case RequestParseState::HeaderSpace:
            if (c == ' ') {
                // skip extra spaces
            } else if (c == '\r') {
                m_headerPairs.addHeaderPair(m_parseHeaderKey, m_parseHeaderValue);
                m_parseHeaderKey.clear();
                m_parseHeaderValue.clear();
                m_parseState = RequestParseState::HeaderCR;
            } else {
                m_parseHeaderValue += c;
                m_parseState = RequestParseState::HeaderValue;
            }
            break;

        case RequestParseState::HeaderValue:
            if (c == '\r') {
                m_headerPairs.addHeaderPair(m_parseHeaderKey, m_parseHeaderValue);
                m_parseHeaderKey.clear();
                m_parseHeaderValue.clear();
                m_parseState = RequestParseState::HeaderCR;
            } else {
                m_parseHeaderValue += c;
            }
            break;

        case RequestParseState::HeaderCR:
            if (c == '\n') {
                m_parseState = RequestParseState::HeaderLF;
            } else {
                return kBadRequest;
            }
            break;

        case RequestParseState::HeaderLF:
            if (c == '\r') {
                m_parseState = RequestParseState::HeaderEndCR;
            } else {
                m_parseHeaderKey += c;
                m_parseState = RequestParseState::HeaderKey;
            }
            break;

        case RequestParseState::HeaderEndCR:
            if (c == '\n') {
                m_parseState = RequestParseState::Done;
                return kIncomplete; // 解析完成
            } else {
                return kBadRequest;
            }
            break;

        case RequestParseState::Done:
            return kIncomplete;
        }
        return kNoError;
    }

    std::pair<HttpErrorCode, ssize_t> HttpRequestHeader::fromString(std::string_view str)
    {
        if (m_parseState == RequestParseState::Done) {
            return {kNoError, 0};
        }
        ssize_t consumed = 0;
        for (char c : str) {
            HttpErrorCode err = parseChar(c);
            ++consumed;
            if (err == kIncomplete) {
                return {kNoError, consumed}; // 解析完成
            } else if (err != kNoError) {
                return {err, -1}; // 解析错误
            }
        }
        return {kNoError, 0}; // 数据不完整
    }

    std::pair<HttpErrorCode, ssize_t> HttpRequestHeader::fromIOVec(const std::vector<iovec>& iovecs)
    {
        if (m_parseState == RequestParseState::Done) {
            return {kNoError, 0};
        }

        // 调用方保证每次传入的buffer都是新数据（已consume过的）
        ssize_t consumed = 0;
        for (size_t iov_idx = 0; iov_idx < iovecs.size(); ++iov_idx) {
            const char* data = static_cast<const char*>(iovecs[iov_idx].iov_base);
            for (size_t i = 0; i < iovecs[iov_idx].iov_len; ++i) {
                HttpErrorCode err = parseChar(data[i]);
                ++consumed;
                ++m_parsedBytes;
                if (err == kIncomplete) {
                    return {kNoError, consumed}; // 解析完成
                } else if (err != kNoError) {
                    return {err, -1}; // 解析错误
                }
            }
        }
        return {kIncomplete, consumed}; // 数据不完整，返回已消费的字节数
    }

    std::string HttpRequestHeader::toString() const
    {
        // 构建 URI（带参数）
        std::string uri_str = m_uri;
        if (!m_argList.empty())
        {
            uri_str += '?';
            int i = 0;
            for (const auto& [k, v] : m_argList)
            {
                uri_str += k;
                uri_str += '=';
                uri_str += v;
                if(i++ < static_cast<int>(m_argList.size()) - 1) {
                    uri_str += '&';
                }
            }
        }
        uri_str = convertToUri(std::move(uri_str));
        
        // 获取方法、版本和头部字符串
        std::string method_str = httpMethodToString(this->m_method);
        std::string version_str = httpVersionToString(this->m_version);
        std::string headers_str = m_headerPairs.toString();
        
        // 预分配结果字符串
        size_t estimated_size = method_str.size() + 1 + uri_str.size() + 1 + 
                                version_str.size() + 2 + headers_str.size() + 2;
        std::string result;
        result.reserve(estimated_size);
        
        // 直接拼接，避免 ostringstream 开销
        result += method_str;
        result += ' ';
        result += uri_str;
        result += ' ';
        result += version_str;
        result += "\r\n";
        result += headers_str;
        result += "\r\n";
        
        return result;
    }

    bool HttpRequestHeader::isKeepAlive() const
    {
        // 使用 hasKey 避免不必要的查找，然后只查找一次
        if (!m_headerPairs.hasKey("Connection")) {
            return false;
        }
        // 只调用一次 getValue，避免重复查找
        const std::string& conn = m_headerPairs.getValue("Connection");
        return conn == "keep-alive";
    }

    bool HttpRequestHeader::isChunked() const
    {
        // 使用 hasKey 避免不必要的查找
        if (!m_headerPairs.hasKey("Transfer-Encoding")) {
            return false;
        }
        const std::string& te = m_headerPairs.getValue("Transfer-Encoding");
        return te == "chunked";
    }

    bool HttpRequestHeader::isConnectionClose() const
    {
        // 使用 hasKey 避免不必要的查找
        if (!m_headerPairs.hasKey("Connection")) {
            return false;
        }
        const std::string& conn = m_headerPairs.getValue("Connection");
        return conn == "close";
    }

    void HttpRequestHeader::copyFrom(const HttpRequestHeader& header)
    {
        this->m_method = header.m_method;
        this->m_uri = header.m_uri;
        this->m_version = header.m_version;
        this->m_argList = header.m_argList;
        this->m_headerPairs = header.m_headerPairs;
    }

    void HttpRequestHeader::reset()
    {
        m_version = HttpVersion::HttpVersion_Unknown;
        m_method = HttpMethod::HttpMethod_Unknown;
        if(!m_uri.empty()) m_uri.clear();
        if(!m_argList.empty()) m_argList.clear();
        m_headerPairs.clear();
        // 重置解析状态
        m_parseState = RequestParseState::Method;
        m_parseMethodStr.clear();
        m_parseUriStr.clear();
        m_parseVersionStr.clear();
        m_parseHeaderKey.clear();
        m_parseHeaderValue.clear();
        m_parsedBytes = 0;
    }

    void HttpRequestHeader::parseArgs(std::string uri)
    {
        size_t argindx = uri.find('?');
        if (argindx != std::string::npos)
        {
            int cur = 0;
            this->m_uri = uri.substr(cur, argindx - cur);
            std::string args = uri.substr(argindx + 1);
            std::string key, value;
            int status = 0;
            for (int i = 0; i < args.length(); i++)
            {
                if (status == 0)
                {
                    if (args[i] == '=')
                    {
                        status = 1;
                    }
                    else
                    {
                        key += args[i];
                    }
                }
                else
                {
                    if (args[i] == '&')
                    {
                        this->m_argList[key] = value;
                        key = "", value = "";
                        status = 0;
                    }
                    else
                    {
                        value += args[i];
                        if (i == args.length() - 1)
                        {
                            this->m_argList[key] = value;
                        }
                    }
                }
            }
        }
    }

    std::string HttpRequestHeader::convertFromUri(std::string_view url, bool convert_plus_to_space)
    {
        std::string result;
        for (size_t i = 0; i < url.size(); i++)
        {
            if (url[i] == '%' && i + 1 < url.size())
            {
                if (url[i + 1] == 'u')
                {
                    auto val = 0;
                    if (fromHexToI(url, i + 2, 4, val))
                    {
                        char buff[4];
                        size_t len = toUtf8(val, buff);
                        if (len > 0)
                        {
                            result.append(buff, len);
                        }
                        i += 5;
                    }
                    else
                    {
                        result += url[i];
                    }
                }
                else
                {
                    auto val = 0;
                    if (fromHexToI(url, i + 1, 2, val))
                    {
                        result += static_cast<char>(val);
                        i += 2;
                    }
                    else
                    {
                        result += url[i];
                    }
                }
            }
            else if (convert_plus_to_space && url[i] == '+')
            {
                result += ' ';
            }
            else
            {
                result += url[i];
            }
        }

        return result;
    }

    std::string HttpRequestHeader::convertToUri(std::string&& url) const
    {
        std::string result;
        size_t n = url.size();
        for (size_t i = 0; i < n ; i++)
        {
            switch (url[i])
            {
            case ' ':
                result += "%20";
                break;
            case '+':
                result += "%2B";
                break;
            case '\r':
                result += "%0D";
                break;
            case '\n':
                result += "%0A";
                break;
            case '\'':
                result += "%27";
                break;
            case ',':
                result += "%2C";
                break;
                // case ':': result += "%3A"; break; // ok? probably...
            case ';':
                result += "%3B";
                break;
            default:
                auto c = static_cast<uint8_t>(url[i]);
                if (c >= 0x80)
                {
                    result += '%';
                    char hex[4];
                    auto len = snprintf(hex, sizeof(hex) - 1, "%02X", c);
                    assert(len == 2);
                    result.append(hex, static_cast<size_t>(len));
                }
                else
                {
                    result += url[i];
                }
                break;
            }
        }

        return std::move(result);

    }

    bool HttpRequestHeader::isHex(const char c, int& v)
    {
        if (0x20 <= c && isdigit(c))
        {
            v = c - '0';
            return true;
        }
        else if ('A' <= c && c <= 'F')
        {
            v = c - 'A' + 10;
            return true;
        }
        else if ('a' <= c && c <= 'f')
        {
            v = c - 'a' + 10;
            return true;
        }
        return false;
    }

    size_t HttpRequestHeader::toUtf8(const int code, char* buff)
    {
        if (code < 0x0080)
        {
            buff[0] = static_cast<char>(code & 0x7F);
            return 1;
        }
        else if (code < 0x0800)
        {
            buff[0] = static_cast<char>(0xC0 | ((code >> 6) & 0x1F));
            buff[1] = static_cast<char>(0x80 | (code & 0x3F));
            return 2;
        }
        else if (code < 0xD800)
        {
            buff[0] = static_cast<char>(0xE0 | ((code >> 12) & 0xF));
            buff[1] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            buff[2] = static_cast<char>(0x80 | (code & 0x3F));
            return 3;
        }
        else if (code < 0xE000)
        {
            return 0;
        }
        else if (code < 0x10000)
        {
            buff[0] = static_cast<char>(0xE0 | ((code >> 12) & 0xF));
            buff[1] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            buff[2] = static_cast<char>(0x80 | (code & 0x3F));
            return 3;
        }
        else if (code < 0x110000)
        {
            buff[0] = static_cast<char>(0xF0 | ((code >> 18) & 0x7));
            buff[1] = static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            buff[2] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            buff[3] = static_cast<char>(0x80 | (code & 0x3F));
            return 4;
        }
        return 0;
    }

    bool HttpRequestHeader::fromHexToI(const std::string_view& s, size_t i, size_t cnt, int& val)
    {
        if (i >= s.size())
        {
            return false;
        }

        val = 0;
        for (; cnt; i++, --cnt)
        {
            if (!s[i])
            {
                return false;
            }
            auto v = 0;
            if (isHex(s[i], v))
            {
                val = val * 16 + v;
            }
            else
            {
                return false;
            }
        }
        return true;
    }

    HttpVersion& HttpResponseHeader::version()
    {
        return this->m_version;
    }

    HttpStatusCode& HttpResponseHeader::code()
    {
        return this->m_code;
    }

    HeaderPair& HttpResponseHeader::headerPairs()
    {
        return this->m_headerPairs;
    }

    bool HttpResponseHeader::isKeepAlive() const
    {
        return m_headerPairs.getValue("Connection") == "keep-alive";
    }

    bool HttpResponseHeader::isChunked() const
    {
        return m_headerPairs.getValue("Transfer-Encoding") == "chunked";
    }

    bool HttpResponseHeader::isConnectionClose() const
    {
        return m_headerPairs.getValue("Connection") == "close";
    }

    std::string HttpResponseHeader::toString() const
    {
        // 获取各部分字符串
        std::string version_str = httpVersionToString(m_version);
        std::string code_str = std::to_string(static_cast<int>(this->m_code));
        std::string status_str = httpStatusCodeToString(m_code);
        std::string headers_str = m_headerPairs.toString();
        
        // 预分配结果字符串
        size_t estimated_size = version_str.size() + 1 + code_str.size() + 1 + 
                                status_str.size() + 2 + headers_str.size() + 2;
        std::string result;
        result.reserve(estimated_size);
        
        // 直接拼接，避免 ostringstream 开销
        result += version_str;
        result += ' ';
        result += code_str;
        result += ' ';
        result += status_str;
        result += "\r\n";
        result += headers_str;
        result += "\r\n";
        
        return result;
    }

    HttpErrorCode HttpResponseHeader::parseChar(char c)
    {
        switch (m_parseState) {
        case ResponseParseState::Version:
            if (c == ' ') {
                m_version = stringToHttpVersion(m_parseVersionStr);
                if (m_version != HttpVersion::HttpVersion_1_0 && m_version != HttpVersion::HttpVersion_1_1) {
                    return kVersionNotSupport;
                }
                m_parseState = ResponseParseState::VersionSP;
            } else if (c == '\r' || c == '\n') {
                return kBadRequest;
            } else {
                m_parseVersionStr += c;
            }
            break;

        case ResponseParseState::VersionSP:
            if (c == ' ') {
                // skip extra spaces
            } else if (c == '\r' || c == '\n') {
                return kBadRequest;
            } else {
                m_parseCodeStr += c;
                m_parseState = ResponseParseState::Code;
            }
            break;

        case ResponseParseState::Code:
            if (c == ' ') {
                try {
                    int code = std::stoi(m_parseCodeStr);
                    m_code = static_cast<HttpStatusCode>(code);
                } catch (...) {
                    return kHttpCodeInvalid;
                }
                m_parseState = ResponseParseState::CodeSP;
            } else if (c == '\r') {
                // HTTP/1.1 200\r\n (no status text)
                try {
                    int code = std::stoi(m_parseCodeStr);
                    m_code = static_cast<HttpStatusCode>(code);
                } catch (...) {
                    return kHttpCodeInvalid;
                }
                m_parseState = ResponseParseState::StatusCR;
            } else if (c == '\n') {
                return kBadRequest;
            } else {
                m_parseCodeStr += c;
            }
            break;

        case ResponseParseState::CodeSP:
            if (c == ' ') {
                // skip extra spaces
            } else if (c == '\r') {
                m_parseState = ResponseParseState::StatusCR;
            } else if (c == '\n') {
                return kBadRequest;
            } else {
                m_parseState = ResponseParseState::Status;
            }
            break;

        case ResponseParseState::Status:
            if (c == '\r') {
                m_parseState = ResponseParseState::StatusCR;
            } else if (c == '\n') {
                return kBadRequest;
            }
            // ignore status text
            break;

        case ResponseParseState::StatusCR:
            if (c == '\n') {
                m_parseState = ResponseParseState::StatusLF;
            } else {
                return kBadRequest;
            }
            break;

        case ResponseParseState::StatusLF:
            if (c == '\r') {
                m_parseState = ResponseParseState::HeaderEndCR;
            } else if (c == '\n') {
                return kBadRequest;
            } else {
                m_parseHeaderKey += c;
                m_parseState = ResponseParseState::HeaderKey;
            }
            break;

        case ResponseParseState::HeaderKey:
            if (c == ':') {
                m_parseState = ResponseParseState::HeaderColon;
            } else if (c == '\r' || c == '\n') {
                return kBadRequest;
            } else {
                m_parseHeaderKey += c;
            }
            break;

        case ResponseParseState::HeaderColon:
            if (c == ' ') {
                m_parseState = ResponseParseState::HeaderSpace;
            } else if (c == '\r') {
                m_headerPairs.addHeaderPair(m_parseHeaderKey, m_parseHeaderValue);
                m_parseHeaderKey.clear();
                m_parseHeaderValue.clear();
                m_parseState = ResponseParseState::HeaderCR;
            } else {
                m_parseHeaderValue += c;
                m_parseState = ResponseParseState::HeaderValue;
            }
            break;

        case ResponseParseState::HeaderSpace:
            if (c == ' ') {
                // skip extra spaces
            } else if (c == '\r') {
                m_headerPairs.addHeaderPair(m_parseHeaderKey, m_parseHeaderValue);
                m_parseHeaderKey.clear();
                m_parseHeaderValue.clear();
                m_parseState = ResponseParseState::HeaderCR;
            } else {
                m_parseHeaderValue += c;
                m_parseState = ResponseParseState::HeaderValue;
            }
            break;

        case ResponseParseState::HeaderValue:
            if (c == '\r') {
                m_headerPairs.addHeaderPair(m_parseHeaderKey, m_parseHeaderValue);
                m_parseHeaderKey.clear();
                m_parseHeaderValue.clear();
                m_parseState = ResponseParseState::HeaderCR;
            } else {
                m_parseHeaderValue += c;
            }
            break;

        case ResponseParseState::HeaderCR:
            if (c == '\n') {
                m_parseState = ResponseParseState::HeaderLF;
            } else {
                return kBadRequest;
            }
            break;

        case ResponseParseState::HeaderLF:
            if (c == '\r') {
                m_parseState = ResponseParseState::HeaderEndCR;
            } else {
                m_parseHeaderKey += c;
                m_parseState = ResponseParseState::HeaderKey;
            }
            break;

        case ResponseParseState::HeaderEndCR:
            if (c == '\n') {
                m_parseState = ResponseParseState::Done;
                return kIncomplete; // 解析完成
            } else {
                return kBadRequest;
            }
            break;

        case ResponseParseState::Done:
            return kIncomplete;
        }
        return kNoError;
    }

    std::pair<HttpErrorCode, ssize_t> HttpResponseHeader::fromString(std::string_view str)
    {
        if (m_parseState == ResponseParseState::Done) {
            return {kNoError, 0};
        }
        ssize_t consumed = 0;
        for (char c : str) {
            HttpErrorCode err = parseChar(c);
            ++consumed;
            if (err == kIncomplete) {
                return {kNoError, consumed}; // 解析完成
            } else if (err != kNoError) {
                return {err, -1}; // 解析错误
            }
        }
        return {kNoError, 0}; // 数据不完整
    }

    std::pair<HttpErrorCode, ssize_t> HttpResponseHeader::fromIOVec(const std::vector<iovec>& iovecs)
    {
        if (m_parseState == ResponseParseState::Done) {
            return {kNoError, 0};
        }

        // 调用方保证每次传入的buffer都是新数据（已consume过的）
        ssize_t consumed = 0;
        for (size_t iov_idx = 0; iov_idx < iovecs.size(); ++iov_idx) {
            const char* data = static_cast<const char*>(iovecs[iov_idx].iov_base);
            for (size_t i = 0; i < iovecs[iov_idx].iov_len; ++i) {
                HttpErrorCode err = parseChar(data[i]);
                ++consumed;
                ++m_parsedBytes;
                if (err == kIncomplete) {
                    return {kNoError, consumed}; // 解析完成
                } else if (err != kNoError) {
                    return {err, -1}; // 解析错误
                }
            }
        }
        return {kIncomplete, consumed}; // 数据不完整，返回已消费的字节数
    }

    void HttpResponseHeader::reset()
    {
        m_code = static_cast<HttpStatusCode>(0);
        m_version = HttpVersion::HttpVersion_Unknown;
        m_headerPairs.clear();
        // 重置解析状态
        m_parseState = ResponseParseState::Version;
        m_parseVersionStr.clear();
        m_parseCodeStr.clear();
        m_parseHeaderKey.clear();
        m_parseHeaderValue.clear();
        m_parsedBytes = 0;
    }

    void HttpResponseHeader::copyFrom(const HttpResponseHeader &header)
    {
        this->m_code = header.m_code;
        this->m_version = header.m_version;
        this->m_headerPairs = header.m_headerPairs;
    }
}