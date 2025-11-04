// HTTPS 测试服务器示例

// ========== 调试开关 ==========
// 取消注释下面这行可以启用所有 debug 日志
// #define ENABLE_DEBUG
// ==================================

#include "galay/kernel/runtime/Runtime.h"
#include "galay-http/server/HttpsServer.h"
#include "galay-http/kernel/http/HttpsRouter.h"
#include "galay-http/kernel/http/HttpsWriter.h"
#include "galay-http/utils/HttpLogger.h"
#include "galay-http/utils/HttpUtils.h"
#include <galay/utils/SignalHandler.hpp>
#include <csignal>
#include <fstream>
#include <iostream>

using namespace galay;
using namespace galay::http;

// 简单的 HTTPS 处理函数
Coroutine<nil> httpsIndex(HttpRequest& request, HttpsConnection& conn, HttpsParams params)
{
    auto writer = conn.getResponseWriter({});
    
    std::string html = R"(
<!DOCTYPE html>
<html>
<head>
    <title>HTTPS Test Server</title>
</head>
<body>
    <h1>HTTPS Test Server</h1>
    <p>This page is served over HTTPS!</p>
    <p>Connection is encrypted with TLS/SSL.</p>
</body>
</html>
)";
    
    auto response = HttpUtils::defaultOk("html", std::move(html));
    co_await writer.reply(response);
    co_await conn.close();
    co_return nil();
}

// API 接口
Coroutine<nil> apiTest(HttpRequest& request, HttpsConnection& conn, HttpsParams params)
{
    auto writer = conn.getResponseWriter({});
    
    std::string json = R"({
    "status": "success",
    "message": "HTTPS API is working!",
    "encrypted": true
})";
    
    auto response = HttpUtils::defaultOk("json", std::move(json));
    co_await writer.reply(response);
    co_await conn.close();
    co_return nil();
}

HttpsRouteMap map = {
    {"/", {httpsIndex}},
    {"/api/test", {apiTest}}
};

int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << "     HTTPS 测试服务器" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "监听地址: https://localhost:8443" << std::endl;
    std::cout << "注意：需要 SSL 证书文件 server.crt 和 server.key" << std::endl;
    std::cout << "按 Ctrl+C 停止服务器" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    // 检查证书文件是否存在
    std::ifstream cert_file("server.crt");
    std::ifstream key_file("server.key");
    
    if (!cert_file.good() || !key_file.good()) {
        std::cerr << "错误：SSL 证书文件不存在！" << std::endl;
        std::cerr << std::endl;
        std::cerr << "请先生成自签名证书：" << std::endl;
        std::cerr << "openssl req -x509 -newkey rsa:4096 -keyout server.key -out server.crt -days 365 -nodes -subj \"/CN=localhost\"" << std::endl;
        std::cerr << std::endl;
        return 1;
    }
    
    // 设置日志级别
    HttpLogger::getInstance()->getLogger()->getSpdlogger()->set_level(spdlog::level::level_enum::debug);
    
    // 注意：TcpSslServer::run() 内部会自动检查并初始化 SSL_CTX
    // 不需要手动调用 initializeSSLServerEnv()
    
    RuntimeBuilder runtimebuilder;
    auto runtime = runtimebuilder.build();
    runtime.start();
    
    HttpsServer server = HttpsServerBuilder("server.crt", "server.key")
                            .addListen(Host("0.0.0.0", 8443))
                            .enableHttp2(false)  // 此测试只支持 HTTP/1.1
                            .build();
    
    HttpsRouter router;
    router.addRoute<GET>(map);
    router.addRoute<POST>(map);
    
    std::cout << "服务器启动成功！" << std::endl;
    std::cout << "使用 curl 测试：" << std::endl;
    std::cout << "  curl -k https://localhost:8443/" << std::endl;
    std::cout << "或在浏览器中访问：https://localhost:8443/" << std::endl;
    std::cout << "(浏览器会显示证书警告，这是正常的，因为使用了自签名证书)" << std::endl;
    std::cout << std::endl;
    
    server.run(runtime, router);
    server.wait();
    
    std::cout << "服务器已停止" << std::endl;
    
    // 注意：不需要手动调用 destroySSLEnv()
    // Galay 框架会在合适的时机自动清理 SSL 环境
    // 手动调用可能导致 double-free 错误
    
    return 0;
}

