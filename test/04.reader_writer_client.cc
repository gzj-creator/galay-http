/**
 * @file test_reader_writer_client.cc
 * @brief HTTP Reader/Writer 测试 - 客户端
 */

#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/common/Log.h"

#ifdef USE_KQUEUE
#include "galay-kernel/kernel/KqueueScheduler.h"
using IOSchedulerType = galay::kernel::KqueueScheduler;
#endif

#ifdef USE_EPOLL
#include "galay-kernel/kernel/EpollScheduler.h"
using IOSchedulerType = galay::kernel::EpollScheduler;
#endif

#ifdef USE_IOURING
#include "galay-kernel/kernel/IOUringScheduler.h"
using IOSchedulerType = galay::kernel::IOUringScheduler;
#endif

using namespace galay::async;
using namespace galay::kernel;

std::atomic<int> g_passed{0};
std::atomic<int> g_failed{0};
std::atomic<bool> g_test_done{false};

// 客户端测试
Coroutine testClient(int test_id, const std::string& path) {
    LogInfo("=== Test #{}: {} ===", test_id, path);

    TcpSocket client;
    client.option().handleNonBlock();

    // 连接到服务器
    Host serverHost(IPType::IPV4, "127.0.0.1", 9999);
    auto connectResult = co_await client.connect(serverHost);
    if (!connectResult) {
        LogError("Test #{} FAILED: Failed to connect: {}", test_id, connectResult.error().message());
        g_failed++;
        co_return;
    }

    LogInfo("Test #{}: Connected to server", test_id);

    // 构造HTTP请求
    std::string requestStr =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: localhost:9999\r\n"
        "User-Agent: galay-http-test/1.0\r\n"
        "Connection: close\r\n"
        "\r\n";

    // 发送请求
    auto sendResult = co_await client.send(requestStr.c_str(), requestStr.size());
    if (!sendResult) {
        LogError("Test #{} FAILED: Failed to send request: {}", test_id, sendResult.error().message());
        g_failed++;
        co_await client.close();
        co_return;
    }

    LogInfo("Test #{}: Request sent: complete", test_id);

    // 接收响应
    char buffer[4096];
    auto recvResult = co_await client.recv(buffer, sizeof(buffer));
    if (!recvResult) {
        LogError("Test #{} FAILED: Failed to receive response: {}", test_id, recvResult.error().message());
        g_failed++;
        co_await client.close();
        co_return;
    }

    auto& bytes = recvResult.value();
    if (bytes.size() == 0) {
        LogError("Test #{} FAILED: Empty response", test_id);
        g_failed++;
        co_await client.close();
        co_return;
    }

    LogInfo("Test #{}: Response received: {} bytes", test_id, bytes.size());
    LogInfo("Test #{}: Response content:\n{}", test_id, bytes.toStringView());

    // 验证响应
    std::string response(bytes.c_str(), bytes.size());
    if (response.find("HTTP/1.1 200 OK") != std::string::npos &&
        response.find("Echo: " + path) != std::string::npos) {
        LogInfo("Test #{} PASSED", test_id);
        g_passed++;
    } else {
        LogError("Test #{} FAILED: Invalid response", test_id);
        g_failed++;
    }

    co_await client.close();
    co_return;
}

// 运行所有测试
Coroutine runAllTests(IOScheduler* scheduler) {
    // 等待一下确保服务器启动
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 启动所有测试（并发执行）
    scheduler->spawn(testClient(1, "/test"));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    scheduler->spawn(testClient(2, "/api/users?id=123"));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    scheduler->spawn(testClient(3, "/very/long/path/to/resource"));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    scheduler->spawn(testClient(4, "/"));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    scheduler->spawn(testClient(5, "/test%20path"));

    // 等待所有测试完成
    std::this_thread::sleep_for(std::chrono::seconds(2));

    g_test_done = true;
    co_return;
}

int main() {
    LogInfo("========================================");
    LogInfo("HTTP Reader/Writer Test - Client");
    LogInfo("========================================\n");

    LogInfo("Make sure the server is running on 127.0.0.1:9999");
    LogInfo("You can start it with: ./test_reader_writer_server\n");

    std::this_thread::sleep_for(std::chrono::seconds(1));

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
    IOSchedulerType scheduler;
    scheduler.start();
    LogInfo("Scheduler started\n");

    // 运行测试
    scheduler.spawn(runAllTests(&scheduler));

    // 等待测试完成
    while (!g_test_done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    scheduler.stop();
    LogInfo("\n========================================");
    LogInfo("Test Results:");
    LogInfo("  Passed: {}", g_passed.load());
    LogInfo("  Failed: {}", g_failed.load());
    LogInfo("  Total:  {}", g_passed.load() + g_failed.load());
    LogInfo("========================================");
#else
    LogWarn("This test requires kqueue (macOS), epoll or io_uring (Linux)");
    return 1;
#endif

    return g_failed > 0 ? 1 : 0;
}
