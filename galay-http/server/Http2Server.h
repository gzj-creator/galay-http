#ifndef GALAY_HTTP2_SERVER_H
#define GALAY_HTTP2_SERVER_H

#include <galay/kernel/server/TcpSslServer.h>
#include "galay-http/kernel/http2/Http2Connection.h"
#include "galay-http/kernel/http2/Http2Callbacks.h"
#include "galay-http/kernel/http2/Http2Params.hpp"

namespace galay::http
{
    // 前向声明
    class HttpsConnection;
    class HttpsRouter;
    struct HttpSettings;
    
    /**
     * @brief HTTP/2 over TLS (h2) 服务器
     * 
     * 这个类专门用于处理 HTTP/2 over TLS 连接。
     * 支持 ALPN 协议协商，如果客户端不支持 HTTP/2，可以降级到 HTTP/1.1。
     * 
     * 使用示例（仅 HTTP/2）:
     * @code
     * Http2Server server = Http2ServerBuilder("server.crt", "server.key")
     *                         .addListen(Host("0.0.0.0", 8443))
     *                         .build();
     * 
     * Http2Callbacks callbacks;
     * callbacks.on_headers = onHeaders;
     * callbacks.on_data = onData;
     * 
     * server.run(runtime, callbacks);
     * server.wait();
     * @endcode
     * 
     * 使用示例（支持降级到 HTTP/1.1）:
     * @code
     * Http2Server server = Http2ServerBuilder("server.crt", "server.key")
     *                         .addListen(Host("0.0.0.0", 8443))
     *                         .enableFallback(true)  // 启用降级
     *                         .build();
     * 
     * Http2Callbacks http2Callbacks;
     * http2Callbacks.on_headers = onHeaders;
     * http2Callbacks.on_data = onData;
     * 
     * HttpsRouter http1Router;
     * // ... 配置 HTTP/1.1 路由 ...
     * 
     * server.run(runtime, http2Callbacks, http1Router);
     * server.wait();
     * @endcode
     */
    class Http2Server
    {
    public:
        using Http2ConnFunc = std::function<Coroutine<nil>(Http2Connection)>;
        using Http1FallbackFunc = std::function<Coroutine<nil>(HttpsConnection&)>;
        
        Http2Server(TcpSslServer&& tcpSslServer, const std::string& cert, const std::string& key)
            : m_server(std::move(tcpSslServer)), m_cert(cert), m_key(key) {}
        
        /**
         * @brief 监听指定地址和端口
         * @param host 主机地址和端口
         */
        void listen(const Host& host);
        
        /**
         * @brief 运行服务器（仅 HTTP/2，不支持降级）
         * @param runtime 运行时环境
         * @param callbacks HTTP/2 帧回调
         * @param params HTTP/2 参数设置
         */
        void run(Runtime& runtime,
                const Http2Callbacks& callbacks,
                Http2Settings params = Http2Settings());
        
        /**
         * @brief 运行服务器（支持降级到 HTTP/1.1 - 使用路由）
         * @param runtime 运行时环境
         * @param http2Callbacks HTTP/2 帧回调
         * @param http1Router HTTP/1.1 路由器（降级时使用）
         * @param http2Params HTTP/2 参数设置
         * @param http1Params HTTP/1.1 参数设置
         */
        void run(Runtime& runtime,
                const Http2Callbacks& http2Callbacks,
                class HttpsRouter& http1Router,
                Http2Settings http2Params = Http2Settings(),
                struct HttpSettings http1Params = HttpSettings());
        
        /**
         * @brief 运行服务器（支持降级到 HTTP/1.1 - 使用自定义处理器）
         * @param runtime 运行时环境
         * @param http2Callbacks HTTP/2 帧回调
         * @param http1Fallback HTTP/1.1 降级处理器
         * @param http2Params HTTP/2 参数设置
         */
        void run(Runtime& runtime,
                const Http2Callbacks& http2Callbacks,
                const Http1FallbackFunc& http1Fallback,
                Http2Settings http2Params = Http2Settings());
        
        /**
         * @brief 运行服务器（使用自定义连接处理器）
         * @param runtime 运行时环境
         * @param handler 连接处理函数
         */
        void run(Runtime& runtime, const Http2ConnFunc& handler);
        
        /**
         * @brief 等待服务器停止
         */
        void wait();
        
        /**
         * @brief 停止服务器
         */
        void stop();
        
    private:
        /**
         * @brief 配置 ALPN 协议
         * @param with_fallback 是否支持降级到 HTTP/1.1
         */
        void configureAlpn(bool with_fallback = false);
        
        /**
         * @brief 处理单个连接（仅 HTTP/2）
         */
        Coroutine<nil> handleConnection(Runtime& runtime,
                                       const Http2Callbacks& callbacks,
                                       const Http2Settings& params,
                                       AsyncSslSocket socket);
        
        /**
         * @brief 处理单个连接（支持降级 - 使用路由）
         */
        Coroutine<nil> handleConnectionWithRouter(Runtime& runtime,
                                                 const Http2Callbacks& http2Callbacks,
                                                 class HttpsRouter& http1Router,
                                                 const Http2Settings& http2Params,
                                                 const struct HttpSettings& http1Params,
                                                 AsyncSslSocket socket);
        
        /**
         * @brief 处理单个连接（支持降级 - 使用自定义处理器）
         */
        Coroutine<nil> handleConnectionWithFallback(Runtime& runtime,
                                                   const Http2Callbacks& http2Callbacks,
                                                   const Http1FallbackFunc& http1Fallback,
                                                   const Http2Settings& http2Params,
                                                   AsyncSslSocket socket);
        
        /**
         * @brief 处理 HTTP/1.1 连接（降级）
         */
        Coroutine<nil> handleHttp1Connection(Runtime& runtime,
                                            class HttpsRouter& router,
                                            const struct HttpSettings& params,
                                            class HttpsConnection& conn);
        
        /**
         * @brief 处理 HTTP/2 帧循环
         */
        Coroutine<nil> processHttp2Frames(Http2Connection& connection,
                                         const Http2Callbacks& callbacks,
                                         const Http2Settings& params);
        
    private:
        TcpSslServer m_server;
        std::string m_cert;
        std::string m_key;
        bool m_alpn_configured = false;
    };
    
    /**
     * @brief HTTP/2 服务器构建器
     */
    class Http2ServerBuilder
    {
    public:
        Http2ServerBuilder(const std::string& cert_file, const std::string& key_file);
        
        /**
         * @brief 添加监听地址
         * @param host 主机地址和端口
         */
        Http2ServerBuilder& addListen(const Host& host);
        
        /**
         * @brief 设置工作线程数
         * @param threads 线程数
         */
        Http2ServerBuilder& threads(int threads);
        
        /**
         * @brief 构建 HTTP/2 服务器
         * @return Http2Server 实例
         */
        Http2Server build();
        
    private:
        std::string m_cert;
        std::string m_key;
        Host m_host = {"0.0.0.0", 8443};  // HTTP/2 over TLS 默认端口 8443
        int m_threads = DEFAULT_COS_SCHEDULER_THREAD_NUM;
    };
}

#endif // GALAY_HTTP2_SERVER_H

