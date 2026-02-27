/**
 * @file B13-H2Client.cc
 * @brief H2 (HTTP/2 over TLS) 客户端压测程序
 */

#include "galay-http/kernel/http2/H2Client.h"
#include "galay-kernel/kernel/Runtime.h"
#include <iostream>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <algorithm>
#include <cstdlib>

#ifdef GALAY_HTTP_SSL_ENABLED

using namespace galay::http2;
using namespace galay::kernel;

static const std::string kEchoPayload = "hello-h2-echo";

std::atomic<int> total_requests{0};
std::atomic<int> success_count{0};
std::atomic<int> fail_count{0};
std::atomic<int> connected_clients{0};
std::atomic<int> connect_failures{0};
std::atomic<int> active_clients{0};

struct ActiveClientGuard {
    ActiveClientGuard() { active_clients.fetch_add(1); }
    ~ActiveClientGuard() { active_clients.fetch_sub(1); }
};

Coroutine runClient(int id,
                    const std::string& host,
                    uint16_t port,
                    int requests_per_client) {
    (void)id;
    ActiveClientGuard guard;

    H2Client client(H2ClientBuilder()
        .verifyPeer(false)
        .build());

    auto connect_result = co_await client.connect(host, port);
    if (!connect_result) {
        connect_failures++;
        fail_count += requests_per_client;
        co_return;
    }

    if (client.getALPNProtocol() != "h2") {
        fail_count += requests_per_client;
        co_await client.close();
        co_return;
    }

    connected_clients++;

    for (int i = 0; i < requests_per_client; i++) {
        auto req_result = co_await client.post("/echo", kEchoPayload, "text/plain");
        total_requests++;
        if (!req_result) {
            fail_count++;
            continue;
        }

        auto response_opt = req_result.value();
        if (!response_opt.has_value()) {
            fail_count++;
            continue;
        }

        auto& response = response_opt.value();
        if (response.status == 200 && response.body == kEchoPayload) {
            success_count++;
        } else {
            fail_count++;
        }
    }

    co_await client.close();
    co_return;
}

void runBenchmark(const std::string& host,
                  uint16_t port,
                  int concurrent_clients,
                  int requests_per_client,
                  int max_wait_seconds,
                  int io_schedulers) {
    total_requests = 0;
    success_count = 0;
    fail_count = 0;
    connected_clients = 0;
    connect_failures = 0;
    active_clients = 0;

    std::cout << "\n========================================\n";
    std::cout << "测试配置:\n";
    std::cout << "  目标服务器: " << host << ":" << port << "\n";
    std::cout << "  并发客户端: " << concurrent_clients << "\n";
    std::cout << "  每客户端请求数: " << requests_per_client << "\n";
    std::cout << "  总请求数: " << (concurrent_clients * requests_per_client) << "\n";
    std::cout << "  IO 调度器线程: " << io_schedulers << "\n";
    std::cout << "========================================\n\n";

    auto start_time = std::chrono::steady_clock::now();
    Runtime runtime(io_schedulers, 0);
    runtime.start();

    for (int i = 0; i < concurrent_clients; i++) {
        auto* scheduler = runtime.getNextIOScheduler();
        scheduler->spawn(runClient(i, host, port, requests_per_client));
    }

    std::cout << "压测进行中";
    int elapsed = 0;
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "." << std::flush;

        if (active_clients.load() == 0) {
            break;
        }

        elapsed += 1;
        if (max_wait_seconds > 0 && elapsed >= max_wait_seconds) {
            std::cerr << "\n[warn] wait timeout, active_clients=" << active_clients.load() << "\n";
            break;
        }
    }
    std::cout << "\n\n";

    runtime.stop();
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    std::cout << "========================================\n";
    std::cout << "测试结果:\n";
    std::cout << "========================================\n";
    std::cout << "连接成功: " << connected_clients << "/" << concurrent_clients << "\n";
    std::cout << "连接失败: " << connect_failures << "\n";
    std::cout << "请求成功: " << success_count << "\n";
    std::cout << "请求失败: " << fail_count << "\n";
    std::cout << "总耗时: " << duration.count() / 1000.0 << "s\n";
    if (duration.count() > 0) {
        double rps = (success_count.load() * 1000.0) / duration.count();
        std::cout << "请求吞吐: " << static_cast<int>(rps) << " req/s\n";
    }
    std::cout << "========================================\n\n";
}

int main(int argc, char* argv[]) {
    std::string host = "localhost";
    uint16_t port = 9443;
    int concurrent_clients = 20;
    int requests_per_client = 20;
    int max_wait_seconds = 60;
    int io_schedulers = 2;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = static_cast<uint16_t>(std::atoi(argv[2]));
    if (argc > 3) concurrent_clients = std::atoi(argv[3]);
    if (argc > 4) requests_per_client = std::atoi(argv[4]);
    if (argc > 5) max_wait_seconds = std::atoi(argv[5]);
    if (argc > 6) io_schedulers = std::max(1, std::atoi(argv[6]));

    runBenchmark(host, port, concurrent_clients, requests_per_client, max_wait_seconds, io_schedulers);
    return 0;
}

#else

int main() {
    std::cout << "SSL support is not enabled.\n";
    std::cout << "Rebuild with -DGALAY_HTTP_ENABLE_SSL=ON\n";
    return 0;
}

#endif
