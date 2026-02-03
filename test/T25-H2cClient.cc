/**
 * @file T25-H2cClient.cc
 * @brief H2c 客户端测试程序
 * @details 用于测试 H2c 客户端功能
 *
 * 使用方法:
 *   ./test/T25-H2cClient <host> <port> [requests]
 *   默认: localhost 9080 10
 */

#include "galay-http/kernel/http2/H2cClient.h"
#include "galay-kernel/kernel/Runtime.h"
#include <iostream>
#include <atomic>

using namespace galay::http2;
using namespace galay::kernel;

std::atomic<int> success_count{0};
std::atomic<int> fail_count{0};

/**
 * @brief 客户端测试协程
 */
Coroutine testClient(const std::string& host, uint16_t port, int num_requests) {
    H2cClient client;

    std::cout << "Connecting to " << host << ":" << port << "..." << std::endl;
    auto connect_result = co_await client.connect(host, port);
    if (!connect_result) {
        std::cerr << "Connect failed: " << connect_result.error().message() << std::endl;
        fail_count++;
        co_return;
    }
    std::cout << "Connected!" << std::endl;

    // Upgrade to HTTP/2
    std::cout << "Upgrading to HTTP/2..." << std::endl;
    while (true) {
        auto upgrade_result = co_await client.upgrade("/");
        if (!upgrade_result) {
            std::cerr << "Upgrade failed with error" << std::endl;
            fail_count++;
            co_return;
        }
        if (upgrade_result.value().has_value()) {
            if (*upgrade_result.value()) {
                std::cout << "Upgrade successful!" << std::endl;
                break;
            } else {
                std::cerr << "Upgrade rejected by server" << std::endl;
                fail_count++;
                co_return;
            }
        }
    }

    // Send multiple GET requests
    std::cout << "Sending " << num_requests << " requests..." << std::endl;
    for (int i = 0; i < num_requests; i++) {
        std::cout << "Starting request " << (i + 1) << "..." << std::endl;
        bool request_success = false;
        int loop_count = 0;

        // 创建一次请求 awaitable 并保存引用
        auto& request_awaitable = client.get("/");

        // 循环等待直到请求完成
        while (true) {
            loop_count++;
            auto result = co_await request_awaitable;

            if (!result) {
                std::cerr << "Request " << (i + 1) << " failed with error" << std::endl;
                fail_count++;
                break;
            }

            if (result.value().has_value()) {
                auto response = result.value().value();
                std::cout << "Request " << (i + 1) << " completed: status=" << response.status
                          << ", body_size=" << response.body.size() << " bytes" << std::endl;
                success_count++;
                request_success = true;
                break;
            }

            if (loop_count > 100) {
                std::cerr << "Request " << (i + 1) << " timeout after " << loop_count << " iterations" << std::endl;
                fail_count++;
                break;
            }
        }

        if (!request_success) {
            std::cerr << "Request " << (i + 1) << " was not successful, breaking loop" << std::endl;
            break;
        }
        std::cout << "Request " << (i + 1) << " finished, continuing to next..." << std::endl;
    }

    // Close connection
    std::cout << "Closing connection..." << std::endl;
    co_await client.close();
    std::cout << "Connection closed." << std::endl;

    co_return;
}

int main(int argc, char* argv[]) {
    std::string host = "localhost";
    uint16_t port = 9080;
    int num_requests = 10;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = std::atoi(argv[2]);
    if (argc > 3) num_requests = std::atoi(argv[3]);

    std::cout << "========================================\n";
    std::cout << "H2c Client Test\n";
    std::cout << "========================================\n";
    std::cout << "Target: " << host << ":" << port << "\n";
    std::cout << "Requests: " << num_requests << "\n";
    std::cout << "========================================\n\n";

    try {
        Runtime runtime(1, 0);
        runtime.start();

        auto* scheduler = runtime.getNextIOScheduler();
        scheduler->spawn(testClient(host, port, num_requests));

        // Wait for completion (max 30 seconds)
        for (int i = 0; i < 300; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (success_count.load() + fail_count.load() >= num_requests) {
                break;
            }
        }

        runtime.stop();

        std::cout << "\n========================================\n";
        std::cout << "Test Results:\n";
        std::cout << "========================================\n";
        std::cout << "Success: " << success_count << "\n";
        std::cout << "Failed: " << fail_count << "\n";
        std::cout << "Total: " << (success_count + fail_count) << "/" << num_requests << "\n";
        std::cout << "========================================\n";

        return (fail_count > 0) ? 1 : 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
