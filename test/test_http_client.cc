
#include "galay-http/client/HttpClient.h"
#include "galay/kernel/runtime/Runtime.h"
#include "galay-http/utils/HttpUtils.h"
#include "galay-http/utils/HttpLogger.h"


using namespace galay;
using namespace galay::http;

Coroutine<nil> test(Runtime& runtime)
{
    std::cout << "test start" << std::endl;
    HttpClient client(runtime, {});
    if(auto res = client.init(); !res) {
        std::cout << "init failed: " << res.error().message() << std::endl;
        co_return nil();
    }
    if(auto res = co_await client.connect({"127.0.0.1", 8080}); !res) {
        std::cout << "connect failed: " << res.error().message() << std::endl;
        co_return nil ();
    }
    std::cout << "connect success" << std::endl;
    auto reader = client.getReader();
    auto writer = client.getWriter();
    HttpRequest request = HttpUtils::defaultGet("/echo");
    auto res = co_await writer.send(request);
    if(!res) {
        std::cout << "send failed: " << res.error().message() << std::endl;
        co_return nil();
    }
    std::cout << "send success" << std::endl;
    auto response = co_await reader.getResponse();
    co_return nil();
}

int main()
{
    HttpLogger::getInstance()->getLogger()->getSpdlogger()->set_level(spdlog::level::level_enum::debug);
    RuntimeBuilder builder;
    auto runtime = builder.build();
    runtime.start();
    runtime.schedule(test(runtime));
    getchar();
    runtime.stop();
    return 0;
}