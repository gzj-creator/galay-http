#ifndef GALAY_HTTP_HEADER_H
#define GALAY_HTTP_HEADER_H

#include "HttpBase.h"
#include "HttpError.h"
#include <string_view>
#include <map>
#include <memory>
#include <vector>
#include <sys/uio.h>


namespace galay::http {

    // 请求头解析状态
    enum class RequestParseState {
        Method,
        MethodSP,
        Uri,
        UriSP,
        Version,
        VersionCR,
        VersionLF,
        HeaderKey,
        HeaderColon,
        HeaderSpace,
        HeaderValue,
        HeaderCR,
        HeaderLF,
        HeaderEndCR,
        Done
    };

    // 响应头解析状态
    enum class ResponseParseState {
        Version,
        VersionSP,
        Code,
        CodeSP,
        Status,
        StatusCR,
        StatusLF,
        HeaderKey,
        HeaderColon,
        HeaderSpace,
        HeaderValue,
        HeaderCR,
        HeaderLF,
        HeaderEndCR,
        Done
    };

    class HeaderPair
    {
    public:
        HeaderPair();
        HeaderPair(const HeaderPair& other);
        HeaderPair(HeaderPair&& other);
        bool hasKey(const std::string& key) const;
        std::string getValue(const std::string& key) const;
        HttpErrorCode removeHeaderPair(const std::string& key);
        HttpErrorCode addHeaderPairIfNotExist(const std::string& key, const std::string& value);
        HttpErrorCode addHeaderPair(const std::string& key, const std::string& value);
        std::string toString() const;
        void clear();
        HeaderPair& operator=(const HeaderPair& other);
        HeaderPair& operator=(HeaderPair&& other);
    private:
        std::map<std::string, std::string> m_headerPairs;
    };

    class HttpRequestHeader
    {
    public:
        HttpRequestHeader() = default;

        friend class HttpRequest;  // 允许HttpRequest访问私有成员

        HttpMethod& method();
        std::string& uri();
        HttpVersion& version();
        std::map<std::string,std::string>& args();
        HeaderPair& headerPairs();
        std::string toString() const;
        bool isKeepAlive() const;
        bool isChunked() const;
        bool isConnectionClose() const;
        bool isHeaderComplete() const { return m_parseState == RequestParseState::Done; }
        /*
            增量解析，返回本次消耗的字节数
            ret.second: >0 解析完成，消耗的字节数; 0 数据不完整需要更多数据; -1 解析错误
            ret.first: kNoError, kBadRequest, kVersionNotSupport
        */
        std::pair<HttpErrorCode, ssize_t> fromString(std::string_view str);
        std::pair<HttpErrorCode, ssize_t> fromIOVec(const std::vector<iovec>& iovecs);
        void copyFrom(const HttpRequestHeader& header);
        void reset();
    private:
        // 解析单个字符，返回错误码，kNoError表示继续，kIncomplete表示完成
        HttpErrorCode parseChar(char c);
        void parseArgs(std::string uri);
        std::string convertFromUri(std::string_view url, bool convert_plus_to_space);
        std::string convertToUri(std::string&& url) const;
        bool isHex(char c, int &v);
        size_t toUtf8(int code, char *buff);
        bool fromHexToI(const std::string_view &s, size_t i, size_t cnt, int &val);
    private:
        HttpMethod m_method;
        std::string m_uri;
        HttpVersion m_version;
        std::map<std::string, std::string> m_argList;
        HeaderPair m_headerPairs;
        // 增量解析状态
        RequestParseState m_parseState = RequestParseState::Method;
        std::string m_parseMethodStr;
        std::string m_parseUriStr;
        std::string m_parseVersionStr;
        std::string m_parseHeaderKey;
        std::string m_parseHeaderValue;
        size_t m_parsedBytes = 0;  // 已经解析的字节数
    };

    class HttpResponseHeader
    {
    public:
        using ptr = std::shared_ptr<HttpResponseHeader>;

        friend class HttpResponse;  // 允许HttpResponse访问私有成员

        HttpVersion& version();
        HttpStatusCode& code();
        HeaderPair& headerPairs();
        bool isKeepAlive() const;
        bool isChunked() const;
        bool isConnectionClose() const;
        bool isHeaderComplete() const { return m_parseState == ResponseParseState::Done; }
        std::string toString() const;
        /*
            增量解析，返回本次消耗的字节数
            ret.second: >0 解析完成，消耗的字节数; 0 数据不完整需要更多数据; -1 解析错误
            ret.first: kNoError, kBadRequest, kVersionNotSupport, kHttpCodeInvalid
        */
        std::pair<HttpErrorCode, ssize_t> fromString(std::string_view str);
        std::pair<HttpErrorCode, ssize_t> fromIOVec(const std::vector<iovec>& iovecs);
        void copyFrom(const HttpResponseHeader& header);
        void reset();
    private:
        HttpErrorCode parseChar(char c);
    private:
        HttpStatusCode m_code;
        HttpVersion m_version;
        HeaderPair m_headerPairs;
        // 增量解析状态
        ResponseParseState m_parseState = ResponseParseState::Version;
        std::string m_parseVersionStr;
        std::string m_parseCodeStr;
        std::string m_parseHeaderKey;
        std::string m_parseHeaderValue;
        size_t m_parsedBytes = 0;  // 已经解析的字节数
    };

}


#endif