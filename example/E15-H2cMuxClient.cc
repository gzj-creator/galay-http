/**
 * @file E15-H2cMuxClient.cc
 * @brief h2c 多路复用客户端示例
 * @details 在一条连接上并发多个 stream，请求乱序返回
 *          使用 StreamManager 进行帧分发，每个流通过 spawn 协程接收响应
 *
 * 测试方法:
 *   ./E15-H2cMuxClient localhost 8080
 */

#include "galay-http/kernel/http2/H2cClient.h"
#include "galay-kernel/kernel/Runtime.h"
#include "galay-kernel/common/Sleep.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

using namespace galay::http2;
using namespace galay::kernel;

static std::atomic<int> g_done_streams{0};
static std::mutex g_mutex;
static std::condition_variable g_cv;
static std::atomic<bool> g_finished{false};

Coroutine handleStream(Http2Stream::ptr stream) {
    std::cout << "[stream " << stream->streamId() << "] waiting response...\n" << std::flush;
    co_await stream->readResponse().wait();
    auto& response = stream->response();

    std::cout << "[stream " << stream->streamId() << "] status=" << response.status
              << " body=\"" << response.body << "\"\n" << std::flush;
    g_done_streams++;
    co_return;
}

Coroutine runClient(const std::string& host, uint16_t port) {
    H2cClient client;

    std::cout << "Connecting to " << host << ":" << port << "...\n" << std::flush;
    auto connect_result = co_await client.connect(host, port);
    if (!connect_result) {
        std::cerr << "Connect failed: " << connect_result.error().message() << "\n";
        g_finished = true;
        g_cv.notify_one();
        co_return;
    }
    std::cout << "Connected!\n" << std::flush;

    std::cout << "Upgrading to HTTP/2...\n" << std::flush;
    co_await client.upgrade("/").wait();
    if (!client.isUpgraded()) {
        std::cerr << "Upgrade failed\n";
        g_finished = true;
        g_cv.notify_one();
        co_return;
    }
    std::cout << "Upgraded to HTTP/2!\n\n" << std::flush;

    auto* mgr = client.getConn()->streamManager();

    std::vector<std::string> paths = {
        "/delay/200",
        "/delay/50",
        "/delay/10",
        "/delay/120",
        "/delay/30"
    };

    for (const auto& path : paths) {
        auto stream = mgr->allocateStream();

        stream->sendHeaders(
            Http2Headers().method("GET").scheme("http")
                .authority(host + ":" + std::to_string(port)).path(path),
            true, true);

        std::cout << "Sent stream " << stream->streamId() << " -> " << path << "\n" << std::flush;
        co_await spawn(handleStream(stream));
    }

    std::cout << "All requests sent, waiting for completion...\n" << std::flush;

    while (g_done_streams < static_cast<int>(paths.size()) && mgr->isRunning()) {
        co_await sleep(std::chrono::milliseconds(10));
    }

    std::cout << "All streams done (" << g_done_streams.load() << "/" << paths.size()
              << "), shutting down...\n" << std::flush;

    if (mgr->isRunning()) {
        // 发送 GOAWAY 通知服务端
        auto goaway_waiter = mgr->sendGoaway();
        if (goaway_waiter) {
            co_await goaway_waiter->wait();
        }
        // 关闭 TCP 连接（非 awaitable），触发 readerLoop 退出
        client.getConn()->initiateClose();
        // 等待 start() 自然完成
        while (mgr->isRunning()) {
            co_await sleep(std::chrono::milliseconds(1));
        }
    }
    std::cout << "Connection closed.\n" << std::flush;

    g_finished = true;
    g_cv.notify_one();
    co_return;
}

int main(int argc, char* argv[]) {
    std::string host = "localhost";
    uint16_t port = 8080;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = std::atoi(argv[2]);

    std::cout << "========================================\n";
    std::cout << "H2c Mux Client Example\n";
    std::cout << "========================================\n";
    std::cout << "Target: " << host << ":" << port << "\n";
    std::cout << "========================================\n\n";

    try {
        Runtime runtime(1, 0);
        runtime.start();

        auto* scheduler = runtime.getNextIOScheduler();
        scheduler->spawn(runClient(host, port));

        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_cv.wait(lock, [] { return g_finished.load(); });
        }

        runtime.stop();

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
