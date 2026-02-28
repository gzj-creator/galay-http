/**
 * @file T28-H2Client.cc
 * @brief H2 (HTTP/2 over TLS) 客户端测试程序
 */

#include "galay-kernel/kernel/Runtime.h"
#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdlib>

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-http/kernel/http2/H2Client.h"
#endif

using namespace galay::kernel;

#ifdef GALAY_HTTP_SSL_ENABLED
using namespace galay::http2;

static std::atomic<int> g_success{0};
static std::atomic<int> g_fail{0};
static std::atomic<bool> g_done{false};

Coroutine runClient(const std::string& host, uint16_t port, int num_requests) {
    H2Client client(H2ClientBuilder()
        .verifyPeer(false)
        .build());

    auto connect_result = co_await client.connect(host, port);
    if (!connect_result) {
        std::cerr << "[connect-fail] " << static_cast<int>(connect_result.error()) << "\n";
        g_fail += num_requests;
        g_done = true;
        co_return;
    }

    std::string alpn = client.getALPNProtocol();
    std::cout << "Negotiated ALPN: " << (alpn.empty() ? "(empty)" : alpn) << "\n";
    if (alpn != "h2") {
        std::cerr << "[alpn-fail] expected h2, got " << alpn << "\n";
        g_fail += num_requests;
        co_await client.close();
        g_done = true;
        co_return;
    }

    for (int i = 0; i < num_requests; i++) {
        auto stream = co_await client.get("/h2/test");
        if (!stream) {
            g_fail++;
            continue;
        }

        auto frame_result = co_await stream->getFrame();
        if (!frame_result || !frame_result.value()) {
            g_fail++;
            continue;
        }

        g_success++;
    }

    co_await client.close();
    g_done = true;
    co_return;
}

int main(int argc, char* argv[]) {
    std::string host = "localhost";
    uint16_t port = 9443;
    int requests = 20;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = static_cast<uint16_t>(std::atoi(argv[2]));
    if (argc > 3) requests = std::atoi(argv[3]);

    std::cout << "========================================\n";
    std::cout << "H2 (HTTP/2 over TLS) Client Test\n";
    std::cout << "========================================\n";
    std::cout << "Target: " << host << ":" << port << "\n";
    std::cout << "Requests: " << requests << "\n";
    std::cout << "========================================\n\n";

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (!scheduler) {
        std::cerr << "No IO scheduler available\n";
        return 1;
    }

    scheduler->spawn(runClient(host, port, requests));

    while (!g_done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    runtime.stop();

    std::cout << "\n========================================\n";
    std::cout << "Test Results\n";
    std::cout << "========================================\n";
    std::cout << "Success: " << g_success.load() << "\n";
    std::cout << "Failed:  " << g_fail.load() << "\n";
    std::cout << "Total:   " << (g_success.load() + g_fail.load()) << "\n";
    std::cout << "========================================\n";

    return g_fail.load() == 0 ? 0 : 1;
}

#else

int main() {
    std::cout << "SSL support is not enabled.\n";
    std::cout << "Rebuild with -DGALAY_HTTP_ENABLE_SSL=ON\n";
    return 0;
}

#endif
