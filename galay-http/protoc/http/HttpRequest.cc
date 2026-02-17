#include "HttpRequest.h"
#include "HttpChunk.h"
#include <algorithm>
#include <cctype>

namespace {

void removeHeaderPairLoose(galay::http::HeaderPair& headers, const std::string& key)
{
    headers.removeHeaderPair(key);

    std::string lower = key;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower != key) {
        headers.removeHeaderPair(lower);
    }

    std::string upper = key;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (upper != key && upper != lower) {
        headers.removeHeaderPair(upper);
    }

    std::string canonical = lower;
    bool word_start = true;
    for (char& ch : canonical) {
        unsigned char c = static_cast<unsigned char>(ch);
        ch = static_cast<char>(word_start ? std::toupper(c) : std::tolower(c));
        word_start = (ch == '-');
    }
    if (canonical != key && canonical != lower && canonical != upper) {
        headers.removeHeaderPair(canonical);
    }
}

std::vector<iovec> sliceIovecs(const std::vector<iovec>& iovecs, size_t skip_bytes)
{
    std::vector<iovec> sliced;
    sliced.reserve(iovecs.size());

    size_t skip = skip_bytes;
    for (const auto& iov : iovecs) {
        if (skip >= iov.iov_len) {
            skip -= iov.iov_len;
            continue;
        }

        const char* base = static_cast<const char*>(iov.iov_base);
        iovec part{};
        part.iov_base = const_cast<char*>(base + skip);
        part.iov_len = iov.iov_len - skip;
        sliced.push_back(part);
        skip = 0;
    }

    return sliced;
}

} // namespace

namespace galay::http
{
    HttpRequestHeader &HttpRequest::header()
    {
       return m_header;
    }

    std::string HttpRequest::getBodyStr()
    {
        return std::move(m_body);
    }

    const std::string& HttpRequest::bodyStr() const
    {
        return m_body;
    }

    void HttpRequest::setHeader(HttpRequestHeader &&header)
    {
        m_header = std::move(header);
    }

    void HttpRequest::setHeader(HttpRequestHeader &header)
    {
        m_header.copyFrom(header);
    }

    void HttpRequest::setBodyStr(std::string &&body)
    {
        m_body = std::move(body);
    }

    std::string HttpRequest::toString()
    {
        if(!m_header.isChunked()) {
            m_header.headerPairs().addHeaderPairIfNotExist("Content-Length", std::to_string(m_body.size()));
        }

        std::string header_str = m_header.toString();

        if(m_header.isChunked()) {
            return header_str;
        }

        // 预分配结果字符串，避免临时字符串
        std::string result;
        result.reserve(header_str.size() + m_body.size());
        result += header_str;
        result += m_body;
        return result;
    }

    std::pair<HttpErrorCode, ssize_t> HttpRequest::fromIOVec(const std::vector<iovec>& iovecs)
    {
        ssize_t newly_consumed = 0;
        size_t header_bytes = 0;  // 本次调用中需要跳过的header字节数

        // 如果header还没解析完，先解析header
        if (!m_headerParsed) {
            auto [err, header_consumed] = m_header.fromIOVec(iovecs);
            if (err != kNoError && err != kIncomplete) {
                return {err, -1};
            }
            if (err == kIncomplete) {
                // header数据不完整，返回已消费的字节数
                return {kNoError, header_consumed};
            }
            // header解析完成 (err == kNoError && header_consumed > 0)
            header_bytes = header_consumed;
            newly_consumed = header_consumed;
            m_headerParsed = true;

            if (m_header.isChunked()) {
                // chunked body 继续往下解析
            } else {
                // header解析完成，获取Content-Length
                std::string content_length_str = m_header.headerPairs().getValue("Content-Length");
                if (content_length_str.empty()) {
                    // 没有body，解析完成
                    return {kNoError, newly_consumed};
                }

                try {
                    m_contentLength = std::stoull(content_length_str);
                } catch (...) {
                    return {kBadRequest, -1};
                }

                if (m_contentLength == 0) {
                    return {kNoError, newly_consumed};
                }
                m_body.reserve(m_contentLength);
            }
        }

        if (m_header.isChunked()) {
            auto body_iovecs = sliceIovecs(iovecs, header_bytes);
            if (body_iovecs.empty()) {
                return {kNoError, newly_consumed};
            }

            auto chunk_result = Chunk::fromIOVec(body_iovecs, m_body);
            if (!chunk_result) {
                if (chunk_result.error().code() == kIncomplete) {
                    return {kNoError, newly_consumed};
                }
                return {chunk_result.error().code(), -1};
            }

            newly_consumed += chunk_result.value().second;

            if (chunk_result.value().first) {
                removeHeaderPairLoose(m_header.headerPairs(), "Transfer-Encoding");
                m_header.headerPairs().addHeaderPair("Content-Length", std::to_string(m_body.size()));
                m_contentLength = m_body.size();
                m_bodyParsed = m_contentLength;
            }

            return {kNoError, newly_consumed};
        }

        // 如果没有body需要解析，直接返回
        if (m_contentLength == 0) {
            return {kNoError, 0};
        }

        size_t iov_idx = 0;
        size_t byte_idx = 0;

        // 跳过header部分（仅当本次调用解析了header时）
        while (header_bytes > 0 && iov_idx < iovecs.size()) {
            size_t available = iovecs[iov_idx].iov_len - byte_idx;
            if (available <= header_bytes) {
                header_bytes -= available;
                ++iov_idx;
                byte_idx = 0;
            } else {
                byte_idx += header_bytes;
                header_bytes = 0;
            }
        }

        // 读取body
        size_t body_needed = m_contentLength - m_bodyParsed;
        size_t body_read = 0;
        while (body_read < body_needed && iov_idx < iovecs.size()) {
            size_t available = iovecs[iov_idx].iov_len - byte_idx;
            size_t to_read = std::min(available, body_needed - body_read);
            m_body.append(static_cast<const char*>(iovecs[iov_idx].iov_base) + byte_idx, to_read);
            body_read += to_read;
            if (to_read == available) {
                ++iov_idx;
                byte_idx = 0;
            } else {
                byte_idx += to_read;
            }
        }

        m_bodyParsed += body_read;
        newly_consumed += body_read;

        return {kNoError, newly_consumed};
    }

    bool HttpRequest::isComplete() const
    {
        if (!m_headerParsed) {
            return false;
        }
        if (m_header.isChunked()) {
            return false; // chunked需要单独处理
        }
        return m_bodyParsed >= m_contentLength;
    }

    void HttpRequest::reset()
    {
        m_header.reset();
        m_body.clear();
        m_contentLength = 0;
        m_bodyParsed = 0;
        m_headerParsed = false;
        m_routeParams.clear();
    }

    // ==================== 路由参数方法实现 ====================
    void HttpRequest::setRouteParams(std::map<std::string, std::string>&& params)
    {
        m_routeParams = std::move(params);
    }

    const std::map<std::string, std::string>& HttpRequest::routeParams() const
    {
        return m_routeParams;
    }

    std::string HttpRequest::getRouteParam(const std::string& name, const std::string& defaultValue) const
    {
        auto it = m_routeParams.find(name);
        return it != m_routeParams.end() ? it->second : defaultValue;
    }

    bool HttpRequest::hasRouteParam(const std::string& name) const
    {
        return m_routeParams.find(name) != m_routeParams.end();
    }
}
