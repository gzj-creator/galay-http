#include "kernel/HttpConnection.h"
#include "utils/HttpUtils.h"
#include <galay/kernel/runtime/Runtime.h>

using namespace galay;

Holder holder;

Coroutine<nil> test_reader(Runtime& runtime)
{
    while (holder.index() == -1);
    
    AsyncTcpSocket socket(runtime);
    TimerGenerator generator(runtime);
    socket.socket();
    socket.options().handleReuseAddr();
    socket.options().handleReusePort();
    socket.bind({"127.0.0.1", 8080});
    socket.listen(1024);
    while (true) { 
        AsyncTcpSocketBuilder builder;
        auto res = co_await socket.accept(builder);
        if(res) {
            auto new_socket = builder.build();
            new_socket.options().handleNonBlock();
            http::HttpConnection connection(std::move(new_socket), std::move(generator));
            auto reader = connection.getRequestReader({});
            auto request = co_await reader.getRequest();
            if(request) {
                std::cout << request.value().toString() << std::endl;
            } else {
                std::cout << request.error().message() << std::endl;
            }
            auto writer = connection.getResponseWriter({});
            auto response = http::HttpUtils::defaultOk("txt", "hello world");
            auto res = co_await writer.reply(response);
            if(res) {
                std::cout << "reply success" << std::endl;
            } else {
                std::cout << res.error().message() << std::endl;
            }
            co_await connection.close();
        } else {
            std::cout << "accept error" << std::endl;
        }
    }
}

int main() { 
    Runtime runtime = RuntimeBuilder().build();
    runtime.start();
    holder = runtime.schedule(test_reader(runtime));
    getchar();
    runtime.stop();
    return 0;
}