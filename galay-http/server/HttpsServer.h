#ifndef GALAY_HTTPS_SERVER_H
#define GALAY_HTTPS_SERVER_H 

#include <galay/kernel/server/TcpSslServer.h>
#include "galay-http/kernel/http/HttpsRouter.h"
#include "galay-http/kernel/http/HttpsConnection.h"
#include "galay-http/kernel/http/HttpParams.hpp"

namespace galay::http 
{

    #define DEFAULT_HTTP2_SERVER_THREAD_NUM 16
    /**
     * @brief HTTPS 服务器（HTTP/1.x over TLS）
     * 
     * 这个类专门用于处理 HTTPS + HTTP/1.x 连接。
     * 如果需要 HTTP/2 over TLS (h2) 支持，请使用 Http2Server 类。
     */
    class HttpsServer 
    {
    public:
        using HttpsConnFunc = std::function<Coroutine<nil>(HttpsConnection)>;
        
        HttpsServer(TcpSslServer&& tcpSslServer, const std::string& cert, const std::string& key) 
            : m_server(std::move(tcpSslServer)), m_cert(cert), m_key(key) {}
        
        /**
         * @brief 监听指定地址和端口
         * @param host 主机地址和端口
         */
        void listen(const Host& host);
        
        /**
         * @brief 运行服务器（自定义连接处理器）
         * @param runtime 运行时环境
         * @param handler 连接处理函数
         */
        void run(Runtime& runtime, const HttpsConnFunc& handler);
        
        /**
         * @brief 运行服务器（使用路由处理 HTTP/1.x 请求）
         * @param runtime 运行时环境
         * @param router HTTP 路由器
         * @param params HTTP 参数设置
         */
        void run(Runtime& runtime, HttpsRouter& router, HttpSettings params = HttpSettings());
        
        /**
         * @brief 等待服务器停止
         */
        void wait();
        
        /**
         * @brief 停止服务器
         */
        void stop();
        
    private:
        Coroutine<nil> handleConnection(CoSchedulerHandle handle, HttpsRouter& router, HttpSettings params, AsyncSslSocket socket);
        
    private:
        TcpSslServer m_server;
        std::string m_cert;
        std::string m_key;
    };

    /**
     * @brief HTTPS 服务器构建器
     */
    class HttpsServerBuilder 
    {
    public:
        HttpsServerBuilder(const std::string& cert_file, const std::string& key_file);
        
        /**
         * @brief 添加监听地址
         * @param host 主机地址和端口
         */
        HttpsServerBuilder& addListen(const Host& host);
        
        /**
         * @brief 设置工作线程数
         * @param threads 线程数
         */
        HttpsServerBuilder& threads(int threads);

        /**
         * @brief 构建 HTTPS 服务器
         * @return HttpsServer 实例
         */
        HttpsServer build();
        
    private:
        std::string m_cert;
        std::string m_key;
        Host m_host = {"0.0.0.0", 8443};  // HTTPS 默认端口 8443
        int m_threads = DEFAULT_HTTP2_SERVER_THREAD_NUM;
    };
}

#endif
