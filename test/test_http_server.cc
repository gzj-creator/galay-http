// ========== 调试开关 ==========
// 取消注释下面这行可以启用所有 debug 日志
// 注意：启用后会严重影响性能！仅用于诊断问题
// #define ENABLE_DEBUG
// ==================================
// #define ENABLE_DEBUG  // 性能测试时注释掉
#include "galay/common/Log.h"
#include "spdlog/common.h"

#include "galay/kernel/runtime/Runtime.h"
#include "kernel/http/HttpRouter.h"
#include "protoc/http/HttpBase.h"
#include "server/HttpServer.h"
#include "utils/HttpLogger.h"
#include "utils/HttpUtils.h"
#include <csignal>
#include <galay/utils/SignalHandler.hpp>
#include <utility>

using namespace galay;
using namespace galay::http;

std::atomic<int> slow_requests = 0;  // 慢请求计数（> 100ms）

Coroutine<nil> test_echo(HttpRequest& request, HttpConnection& conn, HttpParams params)  
{
    auto start = std::chrono::steady_clock::now();
    
    auto writer = conn.getResponseWriter({});
    auto response= HttpUtils::defaultOk("html", "<html>Hello World!</html>");
    
    auto reply_result = co_await writer.reply(response);
    
    // 追踪慢请求
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    
    if(elapsed > 10) {  // 超过 100ms 的请求
        int slow_count = slow_requests.fetch_add(1);
        if(slow_count < 20) {  // 只打印前 20 个慢请求
            LogError("[SLOW REQUEST] took {}ms, status: {}", 
                     elapsed, reply_result ? "success" : reply_result.error().message());
        }
    }
    
    co_return nil();
}

Coroutine<nil> test_static(HttpRequest& request, HttpConnection& conn, HttpParams params)  
{
    auto writer = conn.getResponseWriter({});
    // 获取通配符匹配到的内容
    std::string wildcardContent = params["*"];
    auto response= HttpUtils::defaultOk("txt", "Wildcard matched: " + wildcardContent);
    response.header().headerPairs().addHeaderPair("Connection", "close");
    co_await writer.reply(response);
    co_await conn.close();
    co_return nil();
}

Coroutine<nil> test_params(HttpRequest& request, HttpConnection& conn, HttpParams params)  
{
    auto writer = conn.getResponseWriter({});
    auto response= HttpUtils::defaultOk("txt", std::move(params["id"]));
    response.header().headerPairs().addHeaderPair("Connection", "close");
    co_await writer.reply(response);
    co_await conn.close();
    co_return nil();
}

HttpRouteMap map = {
    {"/", {test_echo}}
};




int main()
{
    HttpLogger::getInstance()->getLogger()->getSpdlogger()->set_level(spdlog::level::err);
    RuntimeBuilder runtimebuilder;
    auto runtime = runtimebuilder.setCoSchedulerNum(8).build();
    runtime.start();
    HttpServerBuilder builder;
    HttpServer server = builder.build();
    server.listen(Host("0.0.0.0", 8080));
    utils::SignalHandler::setSignalHandler<SIGINT>([&server](int signal) {
        server.stop();
    });
    
    HttpRouter router;
    router.addRoute<GET>(map);

    // 配置 HTTP 参数，优化高并发性能
    HttpSettings settings;
    settings.recv_timeout = std::chrono::milliseconds(3000);  // 3 秒接收超时，更快检测断开
    settings.send_timeout = std::chrono::milliseconds(3000);  // 3 秒发送超时
    settings.recv_incr_length = 8192;  // 增加缓冲区步长，减少内存重分配
    
    server.run(runtime, router, settings);
    server.wait();
    return 0;
}