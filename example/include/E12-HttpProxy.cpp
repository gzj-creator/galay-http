#include "example/common/ExampleCommon.h"
#include "galay-http/kernel/http/HttpClient.h"
#include "galay-http/kernel/http/HttpRouter.h"
#include "galay-http/kernel/http/HttpServer.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/utils/Http1_1ResponseBuilder.h"
#include <iostream>
#include <thread>
#include <chrono>

using namespace galay::http;
using namespace galay::kernel;

static Coroutine sendSimpleError(HttpConn& conn, HttpStatusCode code, const std::string& message) {
    auto response = Http1_1ResponseBuilder()
        .status(code)
        .header("Server", "Galay-Proxy-Example/1.0")
        .text(message)
        .build();

    auto writer = conn.getWriter();
    while (true) {
        auto result = co_await writer.sendResponse(response);
        if (!result || result.value()) break;
    }
    co_return;
}

static Coroutine proxyHandler(HttpConn& conn,
                              HttpRequest req,
                              const std::string& upstream_host,
                              uint16_t upstream_port) {
    std::string uri = req.header().uri();
    std::string upstream_url = "http://" + upstream_host + ":" + std::to_string(upstream_port) + uri;

    HttpClient client;
    auto connect_result = co_await client.connect(upstream_url);
    if (!connect_result) {
        co_await sendSimpleError(conn, HttpStatusCode::BadGateway_502, "Bad Gateway: connect upstream failed").wait();
        co_return;
    }

    auto& headers = req.header().headerPairs();
    headers.removeHeaderPair("Host");
    headers.removeHeaderPair("Connection");
    headers.addHeaderPair("Host", upstream_host + ":" + std::to_string(upstream_port));
    headers.addHeaderPair("Connection", "close");

    auto session = client.getSession();
    auto& upstream_writer = session.getWriter();
    while (true) {
        auto send_result = co_await upstream_writer.sendRequest(req);
        if (!send_result) {
            co_await client.close();
            co_await sendSimpleError(conn, HttpStatusCode::BadGateway_502, "Bad Gateway: send upstream failed").wait();
            co_return;
        }
        if (send_result.value()) break;
    }

    HttpResponse upstream_response;
    auto& upstream_reader = session.getReader();
    while (true) {
        auto recv_result = co_await upstream_reader.getResponse(upstream_response);
        if (!recv_result) {
            co_await client.close();
            co_await sendSimpleError(conn, HttpStatusCode::BadGateway_502, "Bad Gateway: recv upstream failed").wait();
            co_return;
        }
        if (recv_result.value()) break;
    }

    co_await client.close();

    auto downstream_writer = conn.getWriter();
    while (true) {
        auto forward_result = co_await downstream_writer.sendResponse(upstream_response);
        if (!forward_result || forward_result.value()) break;
    }

    co_return;
}

int main(int argc, char* argv[]) {
    uint16_t listen_port = example::kDefaultProxyPort;
    std::string upstream_host = "127.0.0.1";
    uint16_t upstream_port = example::kDefaultProxyUpstreamPort;

    if (argc > 1) listen_port = static_cast<uint16_t>(std::atoi(argv[1]));
    if (argc > 2) upstream_host = argv[2];
    if (argc > 3) upstream_port = static_cast<uint16_t>(std::atoi(argv[3]));

    HttpRouter router;
    router.addHandler<HttpMethod::GET, HttpMethod::POST, HttpMethod::PUT,
                      HttpMethod::PATCH, HttpMethod::DELETE, HttpMethod::HEAD,
                      HttpMethod::OPTIONS>("/**",
        [upstream_host, upstream_port](HttpConn& conn, HttpRequest req) -> Coroutine {
            co_await proxyHandler(conn, std::move(req), upstream_host, upstream_port).wait();
            co_return;
        });

    HttpServerConfig config;
    config.host = "0.0.0.0";
    config.port = listen_port;
    config.io_scheduler_count = 2;

    HttpServer server(config);
    std::cout << "Proxy listen : http://127.0.0.1:" << listen_port << "\n";
    std::cout << "Proxy target : http://" << upstream_host << ":" << upstream_port << "\n";
    server.start(std::move(router));

    while (server.isRunning()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}
