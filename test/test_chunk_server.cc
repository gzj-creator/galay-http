// ========== 调试开关 ==========
// 取消注释下面这行可以启用所有 debug 日志
// 注意：启用后会严重影响性能！仅用于诊断问题
// #define ENABLE_DEBUG
// ==================================

#include "galay/kernel/coroutine/CoSchedulerHandle.hpp"
#include "kernel/http/HttpReader.h"
#include "kernel/http/HttpWriter.h"
#include "utils/HttpUtils.h"
#include <galay/kernel/async/AsyncFactory.h>
#include <galay/kernel/runtime/Runtime.h>
#include <iostream>

using namespace galay;
using namespace galay::http;

Coroutine<nil> test_chunk(AsyncTcpSocket&& temp, CoSchedulerHandle handle)
{
    std::cout << "test_chunk" << std::endl;
    AsyncTcpSocket socket = std::move(temp);
    auto generator = handle.getAsyncFactory().getTimerGenerator();
    HttpReader reader(socket, handle, {});
    if(auto res = co_await reader.getRequest(); res) {
        if(res.value().header().isChunked()) {
            if(auto res = co_await reader.getChunkData([](std::string chunk) {
                std::cout << "chunk: " << chunk << std::endl;
            }); !res) {
                std::cout << "getChunkData error: " << res.error().message() << std::endl;
            }
        } else {
            std::cout << "Header: " << res.value().toString() << std::endl;
        }
    } else {
        std::cout << "getRequest error" << std::endl;
        co_return nil();
    }
    

    HttpWriter writer(socket, handle, {});
    auto response = HttpUtils::defaultOk("txt", "");
    if(auto res = co_await writer.replyChunkHeader(response.header()); !res) {
        std::cout << "reply chunk header error: " << res.error().message() << std::endl;
    }
    for(int i = 0; i < 10; ++i) {
        auto res = co_await writer.replyChunkData("hello world", i == 9);
        if(!res) {
            std::cout << "send chunk data error: " << res.error().message() << std::endl;
        }
        std::cout << "chunk data " << i << " sent" << std::endl;
        co_await generator.sleep(std::chrono::milliseconds(1000));
    }
    std::cout << "chunk end" << std::endl;
    co_return nil();
}

Coroutine<nil> test(CoSchedulerHandle handle)
{
    AsyncFactory factory = handle.getAsyncFactory();
    auto socket = factory.getTcpSocket();
    if(auto res = socket.socket(); !res) {
        std::cout << "socket.socket() failed: " << res.error().message() << std::endl;
        co_return nil();
    }
    auto option = socket.options();
    if(auto res = option.handleReusePort(); !res) {
        std::cout << "handle reuse port failed" << std::endl;
        co_return nil();
    }
    if(auto res = option.handleReuseAddr(); !res) {
        std::cout << "handle reuse addr failed" << std::endl;
        co_return nil();
    }
    if(auto res = option.handleReuseAddr(); !res) {
        std::cout << "handle reuse addr failed" << std::endl;
        co_return nil();
    }
    if(auto res = socket.bind({"127.0.0.1", 8080}); !res) {
        std::cout << "bind failed: " << res.error().message() << std::endl;
        co_return nil();
    }
    if(auto res = socket.listen(1024); !res) {
        std::cout << "listen failed: " << res.error().message() << std::endl;
        co_return nil();
    }
    while (true) { 
        AsyncTcpSocketBuilder builder;
        auto res = co_await socket.accept(builder);
        std::cout << "accept" << std::endl;
        if(!res) {
            std::cout << "accept error: " << res.error().message() << std::endl;
            co_return nil();
        }
        auto new_socket = builder.build();
        handle.spawn(test_chunk(std::move(new_socket), handle));
    }
}



int main() { 
    RuntimeBuilder builder;
    auto runtime = builder.build();
    runtime.start();
    runtime.schedule(test(runtime.getCoSchedulerHandle()));
    getchar();
    runtime.stop();
    return 0;
}