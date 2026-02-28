/**
 * @file E14-H2EchoClient.cpp
 * @brief h2 (HTTP/2 over TLS) Echo 客户端示例
 */

#include "galay-kernel/kernel/Runtime.h"
#include <iostream>
#include <cstdlib>
#include <thread>
#include <chrono>

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-http/kernel/http2/H2Client.h"
#endif

#ifdef GALAY_HTTP_SSL_ENABLED

using namespace galay::http2;
using namespace galay::kernel;

Coroutine runClient(const std::string& host, uint16_t port) {
    H2Client client(H2ClientBuilder()
        .verifyPeer(false)
        .build());

    auto connect_result = co_await client.connect(host, port);
    if (!connect_result) {
        std::cerr << "Connect failed: " << static_cast<int>(connect_result.error()) << "\n";
        co_return;
    }

    std::string alpn = client.getALPNProtocol();
    std::cout << "ALPN: " << (alpn.empty() ? "(empty)" : alpn) << "\n";
    if (alpn != "h2") {
        std::cerr << "ALPN is not h2\n";
        co_await client.close();
        co_return;
    }

    std::string body = "Hello from H2EchoClient!";
    auto stream = client.post("/echo", body, "text/plain");
    if (!stream) {
        std::cerr << "Request failed: stream create error\n";
        co_await client.close();
        co_return;
    }

    bool finished = false;
    while (!finished) {
        auto frame_result = co_await stream->getFrame();
        if (!frame_result || !frame_result.value()) {
            break;
        }
        auto frame = std::move(frame_result.value());
        if ((frame->isHeaders() || frame->isData()) && frame->isEndStream()) {
            finished = true;
        }
    }

    if (!finished) {
        std::cerr << "No response\n";
        co_await client.close();
        co_return;
    }

    auto& response = stream->response();
    std::cout << "Status: " << response.status << "\n";
    std::cout << "Body: " << response.body << "\n";

    co_await client.close();
    co_return;
}

int main(int argc, char* argv[]) {
    std::string host = "localhost";
    uint16_t port = 9443;
    if (argc > 1) host = argv[1];
    if (argc > 2) port = static_cast<uint16_t>(std::atoi(argv[2]));

    std::cout << "========================================\n";
    std::cout << "H2 (HTTP/2 over TLS) Echo Client Example\n";
    std::cout << "========================================\n";
    std::cout << "Target: " << host << ":" << port << "\n";
    std::cout << "========================================\n";

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (!scheduler) {
        std::cerr << "No IO scheduler available\n";
        return 1;
    }

    scheduler->spawn(runClient(host, port));
    std::this_thread::sleep_for(std::chrono::seconds(5));
    runtime.stop();
    return 0;
}

#else

int main() {
    std::cout << "SSL support is not enabled.\n";
    std::cout << "Rebuild with -DGALAY_HTTP_ENABLE_SSL=ON\n";
    return 0;
}

#endif
