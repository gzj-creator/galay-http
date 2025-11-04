#ifndef GALAY_HTTPS_SERVER_H
#define GALAY_HTTPS_SERVER_H 

#include <galay/kernel/server/TcpSslServer.h>
#include "galay-http/kernel/http/HttpsRouter.h"
#include "galay-http/kernel/http/HttpsConnection.h"
#include "galay-http/kernel/http/HttpParams.hpp"
#include "galay-http/kernel/http2/Http2Params.hpp"
#include "galay-http/kernel/http2/Http2Callbacks.h"

namespace galay::http 
{
    // 前向声明
    class Http2Connection;
    
    class HttpsServer 
    {
    public:
        using HttpsConnFunc = std::function<Coroutine<nil>(HttpsConnection)>;
        using Http2ConnFunc = std::function<Coroutine<nil>(Http2Connection)>;
        
        HttpsServer(TcpSslServer&& tcpSslServer, const std::string& cert, const std::string& key, bool enable_http2 = true) 
            : m_server(std::move(tcpSslServer)), m_cert(cert), m_key(key), m_http2_enabled(enable_http2), m_ssl_configured(false) {}
        
        void listen(const Host& host);
        
        // 运行服务器（自定义处理器）
        void run(Runtime& runtime, const HttpsConnFunc& handler);
        
        // 运行服务器（使用路由，仅 HTTP/1.1）
        void run(Runtime& runtime, HttpsRouter& router, HttpSettings params = HttpSettings());
        
        // 运行服务器（自动检测 HTTP/2，使用不同的处理器）
        void run(Runtime& runtime, 
                HttpsRouter& http1Router, 
                const Http2ConnFunc& http2Handler,
                HttpSettings httpParams = HttpSettings(),
                Http2Settings http2Params = Http2Settings());
        
        // 运行服务器（自动检测 HTTP/2，使用回调处理帧）
        void run(Runtime& runtime,
                HttpsRouter& http1Router,
                const Http2Callbacks& http2Callbacks,
                HttpSettings httpParams = HttpSettings(),
                Http2Settings http2Params = Http2Settings());
        
        void wait();
        void stop();
        
        /**
         * @brief 启用/禁用 HTTP/2 自动检测
         * @param enabled 是否启用（默认 true）
         */
        void enableHttp2(bool enabled = true) { m_http2_enabled = enabled; }
        
        /**
         * @brief 检查是否启用了 HTTP/2
         */
        bool isHttp2Enabled() const { return m_http2_enabled; }
        
    private:
        // 辅助方法：确保 ALPN 已配置
        void ensureALPNConfigured();
        
        Coroutine<nil> handleConnection(Runtime& runtime, HttpsRouter& router, HttpSettings params, AsyncSslSocket socket);
        Coroutine<nil> handleConnectionWithHttp2(Runtime& runtime, 
                                                 HttpsRouter& http1Router,
                                                 const Http2ConnFunc& http2Handler,
                                                 HttpSettings httpParams,
                                                 Http2Settings http2Params,
                                                 AsyncSslSocket socket);
        Coroutine<nil> handleConnectionWithHttp2Callbacks(Runtime& runtime,
                                                          HttpsRouter& http1Router,
                                                          const Http2Callbacks& http2Callbacks,
                                                          HttpSettings httpParams,
                                                          Http2Settings http2Params,
                                                          AsyncSslSocket socket);
        Coroutine<nil> processHttp2Frames(Http2Connection& connection,
                                          const Http2Callbacks& callbacks,
                                          const Http2Settings& params);
    private:
        TcpSslServer m_server;
        std::string m_cert;
        std::string m_key;
        bool m_http2_enabled = true;  // 默认启用 HTTP/2
        bool m_ssl_configured = false;
    };


    class HttpsServerBuilder 
    {
    public:
        HttpsServerBuilder(const std::string& cert_file, const std::string& key_file);
        HttpsServerBuilder& addListen(const Host& host);
        HttpsServerBuilder& threads(int threads);
        
        /**
         * @brief 启用 HTTP/2 支持（通过 ALPN）
         * @param enabled 是否启用（默认 true）
         */
        HttpsServerBuilder& enableHttp2(bool enabled = true);

        HttpsServer build();
    private:
        std::string m_cert;
        std::string m_key;
        Host m_host = { "0.0.0.0", 8443};  // HTTPS 默认端口 8443
        int m_threads = DEFAULT_COS_SCHEDULER_THREAD_NUM;
        bool m_enable_http2 = true;  // 默认启用 HTTP/2
    };
}

#endif
