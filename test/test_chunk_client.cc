#include "kernel/HttpWriter.h"
#include "kernel/HttpReader.h"
#include "utils/HttpUtils.h"
#include <galay/kernel/async/AsyncFactory.h>

using namespace galay;
using namespace galay::http;

Coroutine<nil> test(Runtime& runtime)
{
    std::cout << "test start" << std::endl;
    AsyncFactory factory(runtime);
    auto socket = factory.createTcpSocket();
    auto generator = factory.createTimerGenerator();
    if(auto res = socket.socket(); !res) {
        std::cout << "socket.socket() failed: " << res.error().message() << std::endl;
        co_return nil();
    }
    auto option = socket.options();
    option.handleNonBlock();
    option.handleReuseAddr();
    option.handleReusePort();
    if(auto res = co_await socket.connect({"127.0.0.1", 8080}); !res) {
        std::cout << "connect failed: " << res.error().message() << std::endl;
        co_return nil();
    }
    HttpWriter writer(socket, generator, {});
    auto wres = co_await writer.sendChunkHeader(HttpUtils::defaultGetHeader("/"));
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
    HttpReader reader(socket, generator, {});
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
    runtime.schedule(test(runtime));
    getchar();
    runtime.stop();
    return 0;
}