// ========== 调试开关 ==========
// 取消注释下面这行可以启用所有 debug 日志
// 注意：启用后会严重影响性能！仅用于诊断问题
// #define ENABLE_DEBUG
// ==================================

#include "galay/kernel/coroutine/CoSchedulerHandle.hpp"
#include "kernel/http/HttpWriter.h"
#include "kernel/http/HttpReader.h"
#include <galay/kernel/runtime/Runtime.h>
#include "utils/HttpUtils.h"
#include <galay/kernel/async/AsyncFactory.h>
#include <iostream>

using namespace galay;
using namespace galay::http;

Coroutine<nil> test(CoSchedulerHandle handle)
{
    std::cout << "test start" << std::endl;
    AsyncFactory factory = handle.getAsyncFactory();
    auto socket = factory.getTcpSocket();
    auto generator = factory.getTimerGenerator();
    if(auto res = socket.socket(); !res) {
        std::cout << "socket.socket() failed: " << res.error().message() << std::endl;
        co_return nil();
    }
    auto option = socket.options();
    if(auto res = option.handleNonBlock(); !res) {
        std::cout << "handle non block failed" << std::endl;
        co_return nil();
    }   
    if(auto res = option.handleReuseAddr(); !res) {
        std::cout << "handle reuse addr failed" << std::endl;
        co_return nil();
    }
    if(auto res = option.handleReusePort(); !res) {
        std::cout << "handle reuse port failed" << std::endl;
        co_return nil();
    }
    if(auto res = co_await socket.connect({"127.0.0.1", 8080}); !res) {
        std::cout << "connect failed: " << res.error().message() << std::endl;
        co_return nil();
    }
    HttpWriter writer(socket, handle, {});
    HttpRequest request = HttpUtils::defaultGet("/");
    auto wres = co_await writer.sendChunkHeader(request.header());
    if(!wres) {
        std::cout << "send chunk header failed: " << wres.error().message() << std::endl;
        co_return nil();
    } else {
        std::cout << "send chunk header success" << std::endl;
    }
    for(int i = 0; i < 10; ++i) {
        auto res = co_await writer.sendChunkData("hello world", i == 9);
        if(!res) {
            std::cout << "send chunk data error: " << res.error().message() << std::endl;
        }
        std::cout << "chunk data " << i << " sent" << std::endl;
        co_await generator.sleep(std::chrono::milliseconds(1000));
    }
    HttpReader reader(socket, handle, {});
    if(auto res = co_await reader.getResponse(); res) {
        if(res.value().header().isChunked()) {
            if(auto res = co_await reader.getChunkData([](std::string chunk) {
                std::cout << "chunk data: " << chunk << std::endl;
            }); !res) {
                std::cout << "get chunk block error: " << res.error().message() << std::endl;
            }
        } else {
            std::cout << "Header: " << res.value().toString() << std::endl;
        }
    } else {
        std::cout << "get response error: " << res.error().message() << std::endl;
    }
    
    co_return nil();
}



int main() { 
    RuntimeBuilder builder;
    auto runtime = builder.build();
    runtime.start();
    runtime.schedule(test(runtime.getCoSchedulerHandle(0).value()));
    getchar();
    runtime.stop();
    return 0;
}