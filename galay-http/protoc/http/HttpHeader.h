#ifndef GALAY_HTTP_HEADER_H
#define GALAY_HTTP_HEADER_H 

#include "HttpBase.h"
#include "HttpError.h"
#include <sstream>
#include <string_view>


namespace galay::http { 
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
        HttpMethod& method();
        std::string& uri();
        HttpVersion& version();
        std::map<std::string,std::string>& args();
        HeaderPair& headerPairs();
        std::string toString() const;
        bool isKeepAlive() const;
        bool isChunked() const;
        bool isConnectionClose() const;
        std::string_view checkAndGetHeaderString(std::string_view str);
        /*
            ret:
                kHttpError_NoError
                kHttpError_BadRequest
                kHttpError_VersionNotSupport
        */
        HttpErrorCode fromString(std::string_view str);
        void copyFrom(const HttpRequestHeader& header);
        void reset();
    private:
        void parseArgs(std::string uri);
        std::string convertFromUri(std::string_view url, bool convert_plus_to_space);
        std::string convertToUri(std::string&& url) const;
        bool isHex(char c, int &v);
        size_t toUtf8(int code, char *buff);
        bool fromHexToI(const std::string_view &s, size_t i, size_t cnt, int &val);
    private:
        HttpMethod m_method;
        std::string m_uri;                                          // uri
        HttpVersion m_version;                                      // 版本号
        std::map<std::string, std::string> m_argList;               // 参数
        HeaderPair m_headerPairs;                                   // 字段
    };

    class HttpResponseHeader
    { 
    public:
        using ptr = std::shared_ptr<HttpResponseHeader>;
        HttpVersion& version();
        HttpStatusCode& code();
        HeaderPair& headerPairs();
        bool isKeepAlive() const;
        bool isChunked() const;
        bool isConnectionClose() const;
        std::string_view checkAndGetHeaderString(std::string_view str);
        std::string toString() const;
        /*
            ret:
                kHttpError_NoError
                kHttpError_BadRequest
                kHttpError_VersionNotSupport
        */
        HttpErrorCode fromString(std::string_view str);
        void copyFrom(const HttpResponseHeader& header);
    private:
        HttpStatusCode m_code;
        HttpVersion m_version;
        HeaderPair m_headerPairs;
    };

}


#endif