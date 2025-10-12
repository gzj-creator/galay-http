#include "galay/kernel/coroutine/Waker.h"
#include "galay/kernel/runtime/Runtime.h"
#include "kernel/HttpRouter.h"
#include "protoc/HttpBase.h"
#include "server/HttpServer.h"
#include "utils/HttpLogger.h"
#include "utils/HttpUtils.h"
#include <utility>

using namespace galay;
using namespace galay::http;

Coroutine<nil> test_echo(HttpRequest& request, HttpReader& reader, HttpWriter& writer, HttpParams params)  
{
    auto response= HttpUtils::defaultOk("txt", "echo");
    co_await writer.reply(response);
    co_return nil();
}

Coroutine<nil> test_static(HttpRequest& request, HttpReader& reader, HttpWriter& writer, HttpParams params)  
{
    auto body = request.header().uri();
    auto response= HttpUtils::defaultOk("txt", std::move(body));
    co_await writer.reply(response);
    co_return nil();
}

Coroutine<nil> test_params(HttpRequest& request, HttpReader& reader, HttpWriter& writer, HttpParams params)  
{
    auto response= HttpUtils::defaultOk("txt", std::move(params["id"]));
    co_await writer.reply(response);
    co_return nil();
}

HttpRouteMap map = {
    {"/echo", {test_echo}},
    {"/static/*", {test_static}},
    {"/endpoint/*/app", {test_static}},
    {"/params/{id}/user", {test_params}}
};

int main()
{
    HttpLogger::getInstance()->getLogger()->getSpdlogger()->set_level(spdlog::level::level_enum::debug);
    RuntimeBuilder runtimebuilder;
    auto runtime = runtimebuilder.build();
    runtime.start();
    HttpServerBuilder builder;
    HttpServer server = builder.build();
    server.listen(Host("0.0.0.0", 8080));
    
    HttpRouter router;
    router.addRoute<GET>(map);
    
    server.run(runtime, router);
    server.wait();
    server.stop();
    return 0;
}