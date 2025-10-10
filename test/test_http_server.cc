#include "server/HttpServer.h"

using namespace galay;
using namespace galay::http;

int main()
{
    HttpServerBuilder builder;
    HttpServer server = builder.build();
    server.listen(Host("0.0.0.0", 8080));
    server.run([](HttpConnection conn, AsyncFactory factory) -> Coroutine<nil> {
        co_return nil();
    });
    return 0;
}