#include "kernel/HttpReader.h"
#include "kernel/HttpWriter.h"
#include "utils/HttpUtils.h"
#include <galay/kernel/async/AsyncFactory.h>

using namespace galay;
using namespace galay::http;

Coroutine<nil> test_chunk(AsyncTcpSocket&& temp, TimerGenerator&& generator)
{
    std::cout << "test_chunk" << std::endl;
    AsyncTcpSocket socket = std::move(temp);
    TimerGenerator gen = std::move(generator);
    HttpReader reader(socket, gen, {});
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
    

    HttpWriter writer(socket, gen, {});
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

Coroutine<nil> test(Runtime& runtime)
{
    AsyncFactory factory = runtime.getAsyncFactory();
    auto socket = factory.getTcpSocket();
    socket.socket();
    socket.options().handleReusePort();
    socket.options().handleReuseAddr();
    socket.bind({"127.0.0.1", 8080});
    socket.listen(1024);
    while (true) { 
        AsyncTcpSocketBuilder builder;
        auto res = co_await socket.accept(builder);
        std::cout << "accept" << std::endl;
        if(!res) {
            std::cout << "accept error: " << res.error().message() << std::endl;
            co_return nil();
        }
        auto new_socket = builder.build();
        runtime.schedule(test_chunk(std::move(new_socket), factory.getTimerGenerator()));
    }
}



int main() { 
    RuntimeBuilder builder;
    auto runtime = builder.build();
    runtime.start();
    runtime.schedule(test(runtime));
    getchar();
    runtime.stop();
    return 0;
}