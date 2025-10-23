#include "HttpHeader.h"

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
        if (m_headerPairs.contains(key))
            return m_headerPairs.at(key);
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
            return kHttpError_HeaderPairNotExist;
        }
        return kHttpError_NoError;
    }

    HttpErrorCode HeaderPair::addHeaderPairIfNotExist(const std::string& key, const std::string& value)
    {
        if (m_headerPairs.contains(key))
        {
            return kHttpError_HeaderPairExist;
        }
        this->m_headerPairs.emplace(key, value);
        return kHttpError_NoError;
    }

    HttpErrorCode HeaderPair::addHeaderPair(const std::string& key, const std::string& value)
    {
        this->m_headerPairs[key] = value;
        return kHttpError_NoError;
    }

    std::string HeaderPair::toString() const
    {
        std::ostringstream stream;
        for (auto& [k, v] : m_headerPairs)
        {
            stream << k << ": " << v << "\r\n";
        }
        return stream.str();
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

    HttpErrorCode HttpRequestHeader::fromString(std::string_view str)
    {
        size_t n = str.size();
        size_t start = 0, end = str.find("\r\n");
        while(end != std::string::npos) {
            if(start == 0) {
                //method
                size_t temp_end = str.find(' ', start);
                if(temp_end == std::string::npos) {
                    return kHttpError_BadRequest;
                }
                m_method = stringToHttpMethod(str.substr(start,  temp_end - start));
                start = temp_end + 1;
                // uri
                temp_end = str.find(' ', start);
                if(temp_end == std::string::npos) {
                    return kHttpError_BadRequest;
                }
                std::string full_uri = convertFromUri(str.substr(start, temp_end - start), false);
                parseArgs(full_uri);  // 解析查询参数，分离路径和参数
                // 如果没有查询参数，parseArgs 不会修改 m_uri，需要手动设置
                if(m_uri.empty()) {
                    m_uri = full_uri;
                }
            
                start = temp_end + 1;
                //version
                m_version = stringToHttpVersion(str.substr(start, end - start));
                if(m_version == HttpVersion::Http_Version_Unknown) {
                    return kHttpError_VersionNotSupport;
                }
            } else {
                std::string_view line;
                std::string key, value;
                line = str.substr(start, end - start);
                if(line.empty()) {
                    break;
                }
                size_t pos = line.find(":");
                if( pos == std::string::npos) {
                    return kHttpError_BadRequest;
                }
                key = line.substr(0, pos);
                if( pos < n - 2 && line[pos + 1] == ' ') value = line.substr(pos + 2);
                else value = line.substr(pos + 1);
                m_headerPairs.addHeaderPair(key, value);
            }
            start = end + 2;
            end = str.find("\r\n", start);
        }
        return kHttpError_NoError;
    }

    std::string HttpRequestHeader::toString() const
    {
        std::ostringstream stream;
        stream << httpMethodToString(this->m_method) << " ";
        std::ostringstream url;
        url << m_uri;
        if (!m_argList.empty())
        {
            url << '?';
            int i = 0;
            for (auto& [k, v] : m_argList)
            {
                url << k << '=' << v ;
                if(i++ < m_argList.size() - 1) {
                    url << '&';
                }
            }
        }
        stream << convertToUri(url.str());
        stream << " " << httpVersionToString(this->m_version) << "\r\n" << m_headerPairs.toString() << "\r\n";
       return stream.str();
    }

    bool HttpRequestHeader::isKeepAlive() const
    {
        return m_headerPairs.getValue("Connection") == "keep-alive";
    }

    bool HttpRequestHeader::isChunked() const
    {
        return m_headerPairs.getValue("Transfer-Encoding") == "chunked";
    }

    bool HttpRequestHeader::isConnectionClose() const
    {
        return m_headerPairs.getValue("Connection") == "close";
    }

    std::string_view HttpRequestHeader::checkAndGetHeaderString(std::string_view str)
    {
        size_t pos = str.find("\r\n\r\n");
        if (pos != std::string::npos)
        {
            return str.substr(0, pos + 4);
        }
        return std::string_view();
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
        m_version = HttpVersion::Http_Version_Unknown;
        m_method = HttpMethod::Http_Method_Unknown;
        if(!m_uri.empty()) m_uri.clear();
        if(!m_argList.empty()) m_argList.clear();
        m_headerPairs.clear();
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

    std::string_view HttpResponseHeader::checkAndGetHeaderString(std::string_view str)
    {
        size_t pos = str.find("\r\n\r\n");
        if (pos != std::string::npos)
        {
            return str.substr(0, pos + 4);
        }
        return std::string_view();
    }


    std::string HttpResponseHeader::toString() const
    {
        std::ostringstream stream;
        stream <<  httpVersionToString(m_version) << ' ' << std::to_string(static_cast<int>(this->m_code)) << ' ' << httpStatusCodeToString(m_code) << "\r\n" \
            << m_headerPairs.toString() << "\r\n";
        return stream.str();
    }

    HttpErrorCode HttpResponseHeader::fromString(std::string_view str)
    {
        size_t n = str.size();
        size_t start = 0, end = str.find("\r\n");
        while(end != std::string::npos) {
            if(start == 0) {
                //method
                size_t temp_end = str.find(' ', start);
                if(temp_end == std::string::npos) {
                    return kHttpError_BadRequest;
                }
                m_version = stringToHttpVersion(str.substr(start,  temp_end - start));
                if(m_version == HttpVersion::Http_Version_Unknown) {
                    return kHttpError_VersionNotSupport;
                }
                start = temp_end + 1;
                // uri
                temp_end = str.find(' ', start);
                if(temp_end == std::string::npos) {
                    return kHttpError_BadRequest;
                }
                int code;
                try
                {
                    std::string code_str;
                    code_str = str.substr(start, temp_end - start);
                    code = std::stoi(code_str);
                }
                catch (std::invalid_argument &e)
                {
                    return kHttpError_HttpCodeInvalid;
                }
                m_code = static_cast<HttpStatusCode>(code);
            } else {
                std::string_view line;
                std::string key, value;
                line = str.substr(start, end - start);
                if(line.empty()) {
                    break;
                }
                size_t pos = line.find(":");
                if(pos == std::string::npos) {
                    return kHttpError_BadRequest;
                }
                key = line.substr(0, pos);
                if( pos < n - 2 && line[pos + 1] == ' ') value = line.substr(pos + 2);
                else value = line.substr(pos + 1);
                m_headerPairs.addHeaderPair(key, value);
            }
            start = end + 2;
            end = str.find("\r\n", start);
        }
        return kHttpError_NoError;
    }

    void HttpResponseHeader::copyFrom(const HttpResponseHeader &header)
    {
        this->m_code = header.m_code;
        this->m_version = header.m_version;
        this->m_headerPairs = header.m_headerPairs;
    }
}