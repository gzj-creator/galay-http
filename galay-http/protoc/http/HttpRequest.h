#ifndef GALAY_HTTP_REQUEST_H
#define GALAY_HTTP_REQUEST_H

#include "HttpHeader.h"
#include "HttpBody.h"
#include <vector>
#include <sys/uio.h>

namespace galay::http 
{ 

    //处理普通 request
    class HttpRequest
    {
    public:
        using ptr = std::shared_ptr<HttpRequest>;
        using wptr = std::weak_ptr<HttpRequest>;
        using uptr = std::unique_ptr<HttpRequest>;

        HttpRequest() = default;
    
        // 添加移动构造函数
        HttpRequest(HttpRequest&&) noexcept = default;
        // 添加移动赋值操作符
        HttpRequest& operator=(HttpRequest&&) noexcept = default;
        
        // 添加析构函数
        ~HttpRequest() = default;
        
        HttpRequestHeader& header();
        //移交所有权
        template<HttpBodyType T>
        T getBody();

        //移交所有权
        std::string getBodyStr();

        void setHeader(HttpRequestHeader&& header);
        void setHeader(HttpRequestHeader& header);
        
        template<HttpBodyType T>
        void setBody(T&& body);
        void setBodyStr(std::string&& body);

        std::string toString();

        // 从离散buffer增量解析，返回消耗的字节数，-1表示解析错误，0表示数据不完整
        std::pair<HttpErrorCode, ssize_t> fromIOVec(const std::vector<iovec>& iovecs);

        // 检查是否解析完成（header + body）
        bool isComplete() const;

        // 重置解析状态
        void reset();

        // ==================== 路由参数支持 ====================
        /**
         * @brief 设置路由参数
         * @param params 路径参数映射（例如 /user/:id 中的 id -> 123）
         */
        void setRouteParams(std::map<std::string, std::string>&& params);

        /**
         * @brief 获取所有路由参数
         * @return 路由参数映射
         */
        const std::map<std::string, std::string>& routeParams() const;

        /**
         * @brief 获取指定的路由参数
         * @param name 参数名
         * @param defaultValue 默认值（参数不存在时返回）
         * @return 参数值
         */
        std::string getRouteParam(const std::string& name, const std::string& defaultValue = "") const;

        /**
         * @brief 检查是否存在指定的路由参数
         * @param name 参数名
         * @return 是否存在
         */
        bool hasRouteParam(const std::string& name) const;

    private:
        std::string m_body;
        HttpRequestHeader m_header;
        // body解析状态
        size_t m_contentLength = 0;
        size_t m_bodyParsed = 0;
        size_t m_headerLength = 0;  // header的字节长度
        bool m_headerParsed = false;
        // 路由参数（由 HttpRouter 设置）
        std::map<std::string, std::string> m_routeParams;
    };

}

#include "HttpRequest.inl"

#endif