/**
 * @file E15-H2cMuxClient.cc
 * @brief h2c 多路复用客户端示例
 * @details 在一条连接上并发多个 stream，请求乱序返回
 *          使用 StreamManager 进行帧分发，每个流通过 stream->getFrame() 协程接收响应
 *
 * 测试方法:
 *   ./E15-H2cMuxClient localhost 8080
 */

#include "galay-http/kernel/http2/H2cClient.h"
#include "galay-kernel/kernel/Runtime.h"
#include "galay-kernel/common/Sleep.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace galay::http2;
using namespace galay::kernel;

static std::atomic<int> g_done_streams{0};

Coroutine handleStream(Http2Stream::ptr stream) {
    co_await stream->readResponse().wait();
    auto& response = stream->response();

    std::cout << "[stream " << stream->streamId() << "] status=" << response.status
              << " body=\"" << response.body << "\"\n";
    g_done_streams++;
    co_return;
}

Coroutine handlePushStream(Http2Stream::ptr stream) {
    while (true) {
        auto frame_result = co_await stream->getFrame();
        if (!frame_result) break;
        auto frame = std::move(frame_result.value());
        if (!frame) break;
        if (frame->isEndStream()) break;
    }
    co_return;
}

Coroutine runClient(const std::string& host, uint16_t port) {
    H2cClient client;

    std::cout << "Connecting to " << host << ":" << port << "...\n";
    auto connect_result = co_await client.connect(host, port);
    if (!connect_result) {
        std::cerr << "Connect failed: " << connect_result.error().message() << "\n";
        co_return;
    }
    std::cout << "Connected!\n";

    auto session = client.getSession();

    std::cout << "Upgrading to HTTP/2...\n";
    auto upgrader = session.upgrade("/");
    while (true) {
        auto result = co_await upgrader();
        if (!result) {
            std::cerr << "Upgrade failed: " << http2ErrorCodeToString(result.error()) << "\n";
            co_await client.close();
            co_return;
        }
        if (result.value()) {
            std::cout << "Upgraded to HTTP/2!\n\n";
            break;
        }
    }

    auto* conn = session.getConn();
    if (!conn) {
        std::cerr << "Failed to get HTTP/2 connection\n";
        co_await client.close();
        co_return;
    }

    // 启动 StreamManager 帧分发循环（等待 writer 就绪后返回）
    auto* mgr = conn->streamManager();
    co_await mgr->startInBackground(handlePushStream).wait();

    std::vector<std::string> paths = {
        "/delay/200",
        "/delay/50",
        "/delay/10",
        "/delay/120",
        "/delay/30"
    };

    // 发送多个请求，每个请求 spawn 一个 handleStream 协程接收响应
    for (const auto& path : paths) {
        auto stream = mgr->allocateStream();

        stream->sendHeaders(
            Http2Headers().method("GET").scheme("http")
                .authority(host + ":" + std::to_string(port)).path(path),
            true, true);

        std::cout << "Sent stream " << stream->streamId() << " -> " << path << "\n";

        // spawn 协程处理该流的响应
        co_await spawn(handleStream(stream));
    }

    // 等待所有流完成
    while (g_done_streams < static_cast<int>(paths.size()) && mgr->isRunning()) {
        co_await sleep(std::chrono::milliseconds(10));
    }

    co_await mgr->shutdown().wait();
    co_await client.close();
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

        std::this_thread::sleep_for(std::chrono::seconds(5));
        runtime.stop();

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
