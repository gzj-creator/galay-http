/**
 * @file test_socket_timeout.cc
 * @brief 直接测试 TcpSocket 超时功能
 */

#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/kernel/Runtime.h"
#include "galay-kernel/common/Log.h"
#include <iostream>
#include <chrono>

using namespace galay::kernel;
using namespace galay::async;
using namespace std::chrono_literals;

Coroutine testSocketTimeout(IOScheduler* scheduler)
{
    std::cout << "=== Testing Socket Connect Timeout ===" << std::endl;

    TcpSocket socket(IPType::IPV4);
    auto nonblock_result = socket.option().handleNonBlock();
    if (!nonblock_result) {
        std::cout << "Failed to set non-block" << std::endl;
        co_return;
    }

    std::cout << "Connecting to 192.0.2.1:9999 with 2s timeout..." << std::endl;

    auto start = std::chrono::steady_clock::now();
    Host host(IPType::IPV4, "192.0.2.1", 9999);
    auto result = co_await socket.connect(host).timeout(2000ms);
    auto end = std::chrono::steady_clock::now();

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "Elapsed time: " << elapsed << " ms" << std::endl;

    if (!result) {
        std::cout << "Connect failed: " << result.error().message() << std::endl;
        std::cout << "Error code: " << result.error().code() << std::endl;

        if (result.error().code() == kTimeout) {
            std::cout << "✓ TIMEOUT DETECTED!" << std::endl;
            if (elapsed >= 1800 && elapsed <= 2500) {
                std::cout << "✓ TIMEOUT DURATION CORRECT!" << std::endl;
            } else {
                std::cout << "⚠ Timeout duration off (expected ~2000ms)" << std::endl;
            }
        } else {
            std::cout << "❌ Expected timeout but got different error" << std::endl;
        }
    } else {
        std::cout << "❌ Connect should have timed out" << std::endl;
    }

    std::cout << std::endl;
    co_return;
}

Coroutine testSocketNoTimeout(IOScheduler* scheduler)
{
    std::cout << "=== Testing Socket Connect Without Timeout ===" << std::endl;

    TcpSocket socket(IPType::IPV4);
    auto nonblock_result = socket.option().handleNonBlock();
    if (!nonblock_result) {
        std::cout << "Failed to set non-block" << std::endl;
        co_return;
    }

    std::cout << "Connecting to 127.0.0.1:9999 (should fail quickly)..." << std::endl;

    auto start = std::chrono::steady_clock::now();
    Host host(IPType::IPV4, "127.0.0.1", 9999);
    auto result = co_await socket.connect(host);
    auto end = std::chrono::steady_clock::now();

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "Elapsed time: " << elapsed << " ms" << std::endl;

    if (!result) {
        std::cout << "Connect failed: " << result.error().message() << std::endl;
        std::cout << "Error code: " << result.error().code() << std::endl;

        if (elapsed < 100) {
            std::cout << "✓ Failed quickly as expected" << std::endl;
        }
    } else {
        std::cout << "⚠ Connect succeeded (port might be open)" << std::endl;
    }

    std::cout << std::endl;
    co_return;
}

int main()
{
    std::cout << "======================================" << std::endl;
    std::cout << "Socket Timeout Verification Test" << std::endl;
    std::cout << "======================================" << std::endl;
    std::cout << std::endl;

    try {
        Runtime runtime;
        runtime.start();

        auto* scheduler = runtime.getNextIOScheduler();
        if (!scheduler) {
            std::cerr << "No IO scheduler available" << std::endl;
            return 1;
        }

        // Test 1: Connect with timeout
        scheduler->spawn(testSocketTimeout(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(4));

        // Test 2: Connect without timeout
        scheduler->spawn(testSocketNoTimeout(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        runtime.stop();

        std::cout << "======================================" << std::endl;
        std::cout << "Tests Completed" << std::endl;
        std::cout << "======================================" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
