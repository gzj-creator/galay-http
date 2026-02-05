/**
 * @file B8-H2cClient.cc
 * @brief HTTP/2 Cleartext (h2c) 客户端压力测试
 * @details 高并发 H2c 客户端，用于压测服务器
 */

#include "galay-http/kernel/http2/H2cClient.h"
#include "galay-kernel/kernel/Runtime.h"
#include <iostream>
#include <atomic>
#include <chrono>

using namespace galay::http2;
using namespace galay::kernel;

// 统计数据
std::atomic<int> total_requests{0};
std::atomic<int> success_count{0};
std::atomic<int> fail_count{0};
std::atomic<int> connected_clients{0};
std::atomic<int> upgrade_failures{0};

/**
 * @brief 单个客户端协程
 */
Coroutine runClient(int client_id, const std::string& host, uint16_t port, int requests_per_client) {
    H2cClient client;

    // 连接到服务器
    auto connect_result = co_await client.connect(host, port);
    if (!connect_result) {
        fail_count++;
        co_return;
    }

    // 升级到 HTTP/2
    bool upgraded = false;
    while (true) {
        auto result = co_await client.upgrade("/");
        if (!result) {
            upgrade_failures++;
            fail_count++;
            co_return;
        }
        if (result.value().has_value()) {
            if (*result.value()) {
                upgraded = true;
                break;
            } else {
                upgrade_failures++;
                fail_count++;
                co_return;
            }
        }
    }

    if (!upgraded) {
        fail_count++;
        co_return;
    }

    connected_clients++;

    // 发送多个请求
    for (int i = 0; i < requests_per_client; i++) {
        bool request_success = false;
        while (true) {
            auto result = co_await client.get("/");
            if (!result) {
                fail_count++;
                break;
            }
            if (result.value().has_value()) {
                // 响应完成
                success_count++;
                total_requests++;
                request_success = true;
                break;
            }
            // result.value() 是 std::nullopt，继续等待
        }

        if (!request_success) {
            break;
        }
    }

    // 关闭连接
    co_await client.close();
    co_return;
}

/**
 * @brief 运行压测
 */
void runBenchmark(const std::string& host, uint16_t port, int concurrent_clients, int requests_per_client) {
    std::cout << "\n========================================\n";
    std::cout << "测试配置:\n";
    std::cout << "  目标服务器: " << host << ":" << port << "\n";
    std::cout << "  并发客户端: " << concurrent_clients << "\n";
    std::cout << "  每客户端请求数: " << requests_per_client << "\n";
    std::cout << "  总请求数: " << (concurrent_clients * requests_per_client) << "\n";
    std::cout << "========================================\n\n";

    // 重置计数器
    total_requests = 0;
    success_count = 0;
    fail_count = 0;
    connected_clients = 0;
    upgrade_failures = 0;

    auto start_time = std::chrono::steady_clock::now();

    // 创建 Runtime
    Runtime runtime(4, 0);  // 4个IO调度器
    runtime.start();

    // 启动所有客户端
    auto* scheduler = runtime.getNextIOScheduler();
    for (int i = 0; i < concurrent_clients; i++) {
        scheduler->spawn(runClient(i, host, port, requests_per_client));
    }

    // 等待所有客户端完成
    std::cout << "压测进行中";
    for (int i = 0; i < 60; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "." << std::flush;

        // 检查是否所有请求都完成
        if (success_count.load() + fail_count.load() >= concurrent_clients * requests_per_client) {
            break;
        }
    }
    std::cout << "\n\n";

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

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

    if (argc > 1) host = argv[1];
    if (argc > 2) port = std::atoi(argv[2]);
    if (argc > 3) concurrent_clients = std::atoi(argv[3]);
    if (argc > 4) requests_per_client = std::atoi(argv[4]);

    std::cout << "========================================\n";
    std::cout << "HTTP/2 Cleartext (h2c) 客户端压测\n";
    std::cout << "========================================\n";
    std::cout << "使用方法: " << argv[0] << " <host> <port> <并发数> <每客户端请求数>\n";
    std::cout << "示例: " << argv[0] << " localhost 9080 100 50\n";
    std::cout << "========================================\n";

    try {
        runBenchmark(host, port, concurrent_clients, requests_per_client);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
