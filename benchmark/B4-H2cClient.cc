/**
 * @file B4-H2cClient.cc
 * @brief HTTP/2 Cleartext (h2c) Echo 客户端压力测试
 * @details 高并发 H2c 客户端，利用 HTTP/2 多路复用在单连接上并发多个 stream
 *          使用 StreamManager 进行帧分发，每个流通过 stream->getFrame() 协程接收响应
 */

#include "galay-http/kernel/http2/H2cClient.h"
#include "galay-kernel/kernel/Runtime.h"
#include <iostream>
#include <atomic>
#include <chrono>
#include <memory>

using namespace galay::http2;
using namespace galay::kernel;

// 统计数据
std::atomic<int> total_requests{0};
std::atomic<int> success_count{0};
std::atomic<int> fail_count{0};
std::atomic<int> connected_clients{0};
std::atomic<int> upgrade_failures{0};
std::atomic<int> connect_failures{0};
std::atomic<int> response_failures{0};
std::atomic<int> active_clients{0};

static const std::string kEchoPayload = "hello-h2c-echo";

struct ActiveClientGuard {
    ActiveClientGuard() { active_clients.fetch_add(1); }
    ~ActiveClientGuard() { active_clients.fetch_sub(1); }
};

Coroutine handleStream(Http2Stream::ptr stream) {
    co_await stream->readResponse().wait();
    auto& response = stream->response();

    if (response.status == 200 && response.body == kEchoPayload) {
        success_count++;
        co_return;
    }

    int resp_idx = response_failures.fetch_add(1);
    if (resp_idx < 5) {
        std::cerr << "[resp-fail] status=" << response.status
                  << " body_len=" << response.body.size() << "\n";
    }
    fail_count++;
    co_return;
}

/**
 * @brief 单个客户端协程 - 使用 HTTP/2 多路复用
 */
Coroutine runClient(std::shared_ptr<H2cClient> client,
                    int client_id,
                    const std::string& host,
                    uint16_t port,
                    int requests_per_client) {
    ActiveClientGuard guard;

    // 连接到服务器
    auto connect_result = co_await client->connect(host, port);
    if (!connect_result) {
        int idx = connect_failures.fetch_add(1);
        if (idx < 5) {
            std::cerr << "[connect-fail] " << connect_result.error().message() << "\n";
        }
        fail_count += requests_per_client;
        co_await client->shutdown().wait();
        co_return;
    }

    // 升级到 HTTP/2（内部启动 StreamManager）
    co_await client->upgrade("/").wait();
    if (!client->isUpgraded()) {
        int idx = upgrade_failures.fetch_add(1);
        if (idx < 5) {
            std::cerr << "[upgrade-fail]\n";
        }
        fail_count += requests_per_client;
        co_await client->shutdown().wait();
        co_return;
    }

    connected_clients++;

    auto* mgr = client->getConn()->streamManager();

    // 发送请求
    for (int i = 0; i < requests_per_client; i++) {
        auto stream = mgr->allocateStream();

        stream->sendHeaders(
            Http2Headers().method("POST").scheme("http")
                .authority(host + ":" + std::to_string(port)).path("/echo")
                .contentType("text/plain").contentLength(kEchoPayload.size()),
            false, true);
        stream->sendData(kEchoPayload, true);
        total_requests++;

        co_await handleStream(stream).wait();
    }

    co_await client->shutdown().wait();
    co_return;
}

/**
 * @brief 运行压测
 */
void runBenchmark(const std::string& host,
                  uint16_t port,
                  int concurrent_clients,
                  int requests_per_client,
                  int max_wait_seconds,
                  int io_schedulers) {
    std::cout << "\n========================================\n";
    std::cout << "测试配置:\n";
    std::cout << "  目标服务器: " << host << ":" << port << "\n";
    std::cout << "  并发客户端: " << concurrent_clients << "\n";
    std::cout << "  每客户端请求数: " << requests_per_client << "\n";
    std::cout << "  总请求数: " << (concurrent_clients * requests_per_client) << "\n";
    std::cout << "  IO 调度器线程: " << io_schedulers << "\n";
    if (max_wait_seconds <= 0) {
        std::cout << "  最大等待: 无限\n";
    } else {
        std::cout << "  最大等待: " << max_wait_seconds << "s\n";
    }
    std::cout << "========================================\n\n";

    // 重置计数器
    total_requests = 0;
    success_count = 0;
    fail_count = 0;
    connected_clients = 0;
    upgrade_failures = 0;
    active_clients = 0;

    auto start_time = std::chrono::steady_clock::now();

    // 创建 Runtime
    Runtime runtime(io_schedulers, 0);
    runtime.start();

    // 客户端对象统一延迟释放，避免 kqueue 队列中残留事件访问已销毁 controller。
    std::vector<std::shared_ptr<H2cClient>> client_pool;
    client_pool.reserve(concurrent_clients);

    // 启动所有客户端（负载均衡到不同的调度器）
    for (int i = 0; i < concurrent_clients; i++) {
        auto client = std::make_shared<H2cClient>(H2cClientBuilder().buildConfig());
        client_pool.push_back(client);
        auto* scheduler = runtime.getNextIOScheduler();
        scheduler->spawn(runClient(std::move(client), i, host, port, requests_per_client));
    }

    // 等待所有客户端完成
    std::cout << "压测进行中";
    int elapsed = 0;
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "." << std::flush;

        // 所有客户端协程都已退出（正常完成或提前失败）
        if (active_clients.load() == 0) {
            break;
        }

        elapsed += 1;
        if (max_wait_seconds > 0 && elapsed >= max_wait_seconds) {
            std::cerr << "\n[warn] wait timeout, active_clients=" << active_clients.load()
                      << ", done=" << (success_count.load() + fail_count.load())
                      << "/" << (concurrent_clients * requests_per_client) << "\n";
            break;
        }
    }
    std::cout << "\n\n";

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    if (active_clients.load() > 0) {
        const auto grace_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (active_clients.load() > 0 && std::chrono::steady_clock::now() < grace_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    runtime.stop();

    // 输出结果
    std::cout << "========================================\n";
    std::cout << "测试结果:\n";
    std::cout << "========================================\n";
    std::cout << "连接成功: " << connected_clients << "/" << concurrent_clients << "\n";
    std::cout << "升级失败: " << upgrade_failures << "\n";
    std::cout << "请求成功: " << success_count << "\n";
    std::cout << "请求失败: " << fail_count << "\n";
    std::cout << "总耗时: " << duration.count() / 1000.0 << "s\n";

    if (duration.count() > 0) {
        double rps = (success_count.load() * 1000.0) / duration.count();
        double cps = (connected_clients.load() * 1000.0) / duration.count();
        std::cout << "请求吞吐: " << static_cast<int>(rps) << " req/s\n";
        std::cout << "连接速率: " << static_cast<int>(cps) << " conn/s\n";

        if (success_count.load() > 0) {
            double success_rate = (success_count.load() * 100.0) / (concurrent_clients * requests_per_client);
            std::cout << "成功率: " << success_rate << "%\n";
        }
    }

    std::cout << "========================================\n\n";
}

int main(int argc, char* argv[]) {
    std::string host = "localhost";
    uint16_t port = 9080;
    int concurrent_clients = 100;
    int requests_per_client = 50;
    int max_wait_seconds = 60;
    int io_schedulers = 1;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = std::atoi(argv[2]);
    if (argc > 3) concurrent_clients = std::atoi(argv[3]);
    if (argc > 4) requests_per_client = std::atoi(argv[4]);
    if (argc > 5) max_wait_seconds = std::atoi(argv[5]);
    if (argc > 6) io_schedulers = std::max(1, std::atoi(argv[6]));

    std::cout << "========================================\n";
    std::cout << "HTTP/2 Cleartext (h2c) 客户端压测\n";
    std::cout << "========================================\n";
    std::cout << "使用方法: " << argv[0] << " <host> <port> <并发数> <每客户端请求数> [最大等待秒数] [io线程数]\n";
    std::cout << "示例: " << argv[0] << " localhost 9080 100 50 60 1\n";
    std::cout << "========================================\n";

    try {
        runBenchmark(host, port, concurrent_clients, requests_per_client, max_wait_seconds, io_schedulers);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
