#ifndef GALAY_HTTPS_CONNECTION_H
#define GALAY_HTTPS_CONNECTION_H 

#include <galay/kernel/async/Socket.h>
#include <galay/kernel/async/TimerGenerator.h>
#include "HttpParams.hpp"
#include "galay/kernel/coroutine/CoSchedulerHandle.hpp"
#include <unordered_map>
#include <string>

// 前向声明
namespace galay::http 
{
    class HttpsReader;
    class HttpsWriter;
    class WsConnection;
    class Http2Connection;
}

namespace galay::http
{ 
    class HttpsConnection 
    {
        friend class WsConnection;
        friend class Http2Connection;
    public:
        HttpsConnection(AsyncSslSocket&& socket, CoSchedulerHandle handle);
        HttpsConnection(HttpsConnection&& other);

        HttpsReader getRequestReader(const HttpSettings& params);
        HttpsWriter getResponseWriter(const HttpSettings& params);
        
        AsyncResult<std::expected<void, CommonError>> close();
        bool isClosed() const;
        // 标记连接为已关闭，不执行任何 I/O 操作（用于连接已被对端关闭的情况）
        void markClosed();
        
        /**
         * @brief 获取 ALPN 协商的协议
         * @return 协议名称（如 "h2", "http/1.1"），如果未协商则返回空字符串
         */
        std::string getAlpnProtocol() const;
        
        /**
         * @brief 检测是否通过 ALPN 协商了 HTTP/2
         * @return true 如果协商的协议是 h2
         */
        bool isHttp2() const;
        
        /**
         * @brief 获取底层 SSL 对象（用于高级操作）
         * @return SSL* 指针
         */
        SSL* getSSL() const;
        
        ~HttpsConnection() = default;
    private:
        bool m_is_closed = false;
        AsyncSslSocket m_socket;
        CoSchedulerHandle m_handle;
        std::unordered_map<std::string, std::string> m_params;
    };
}


#endif

