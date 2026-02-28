/**
 * @file B11-H2cMuxClient.cc
 * @brief HTTP/2 多路复用客户端压力测试
 * @details 与 B4 的区别：B4 在每条连接上串行发送请求，
 *          本程序在每条连接上并发 N 个 stream，真正压测 HTTP/2 多路复用性能
 *
 * 使用方法:
 *   ./B11-H2cMuxClient [host] [port] [connections] [streams_per_conn] [rounds]
 *   默认: localhost 9080 10 100 5
 *
 * 示例:
 *   ./B11-H2cMuxClient localhost 9080 10 100 5   # 10连接 × 100并发流 × 5轮 = 5000请求
 *   ./B11-H2cMuxClient localhost 9080 50 200 10  # 50连接 × 200并发流 × 10轮 = 100000请求
 */

#include "galay-http/kernel/http2/H2cClient.h"
#include "galay-kernel/kernel/Runtime.h"
#include <iostream>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <memory>
#include <vector>

using namespace galay::http2;
using namespace galay::kernel;

static const std::string kPayload = "hello-h2c-mux";

struct Stats {
    std::atomic<int64_t> success{0};
    std::atomic<int64_t> fail{0};
    std::atomic<int>     connected{0};
    std::atomic<int>     active{0};
    std::atomic<int>     connect_err{0};
    std::atomic<int>     upgrade_err{0};
};

static Stats g;

/**
 * 单个 stream 的响应处理协程（由 spawn 并发运行）
 */
Coroutine handleResponse(Http2Stream::ptr stream) {
    co_await stream->readResponse().wait();
    auto& resp = stream->response();

    if (resp.status == 200 && resp.body == kPayload) {
        g.success++;
    } else {
        g.fail++;
    }
    co_return;
}

/**
 * 单条连接的工作协程：
 *   每轮在同一连接上并发发射 streams_per_conn 个 stream，
 *   等待全部完成后进入下一轮
 */
Coroutine runConnection(std::shared_ptr<H2cClient> client,
                        int id,
                        const std::string& host, uint16_t port,
                        int streams_per_conn, int rounds) {
    g.active++;

    auto conn_result = co_await client->connect(host, port);
    if (!conn_result) {
        if (g.connect_err.fetch_add(1) < 3) {
            std::cerr << "[conn " << id << "] connect failed: "
                      << conn_result.error().message() << "\n";
        }
        g.fail += static_cast<int64_t>(streams_per_conn) * rounds;
        co_await client->shutdown().wait();
        g.active--;
        co_return;
    }

    co_await client->upgrade("/").wait();
    if (!client->isUpgraded()) {
        if (g.upgrade_err.fetch_add(1) < 3) {
            std::cerr << "[conn " << id << "] upgrade failed\n";
        }
        g.fail += static_cast<int64_t>(streams_per_conn) * rounds;
        co_await client->shutdown().wait();
        g.active--;
        co_return;
    }

    g.connected++;
    auto* mgr = client->getConn()->streamManager();
    std::string authority = host + ":" + std::to_string(port);

    for (int r = 0; r < rounds && mgr->isRunning(); r++) {
        // 并发发射一批 stream
        std::vector<Coroutine> waiters;
        waiters.reserve(streams_per_conn);

        for (int s = 0; s < streams_per_conn; s++) {
            auto stream = mgr->allocateStream();
            stream->sendHeaders(
                Http2Headers()
                    .method("POST").scheme("http")
                    .authority(authority).path("/echo")
                    .contentType("text/plain")
                    .contentLength(kPayload.size()),
                false, true);
            stream->sendData(kPayload, true);

            Coroutine coro = handleResponse(stream);
            co_await spawn(coro);
            waiters.push_back(std::move(coro));
        }

        // 等待本轮所有 stream 完成
        for (auto& w : waiters) {
            co_await w.wait();
        }
    }

    co_await client->shutdown().wait();
    g.active--;
    co_return;
}

void printResults(int connections, int streams_per_conn, int rounds,
                  std::chrono::milliseconds elapsed) {
    int64_t total = g.success.load() + g.fail.load();
    double sec = elapsed.count() / 1000.0;
    double rps = sec > 0 ? total / sec : 0;
    double success_rate = total > 0 ? g.success.load() * 100.0 / total : 0;

    std::cout << "\n========================================\n";
    std::cout << "测试结果\n";
    std::cout << "========================================\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "连接: " << g.connected.load() << "/" << connections << "\n";
    std::cout << "成功: " << g.success.load() << "\n";
    std::cout << "失败: " << g.fail.load() << "\n";
    std::cout << "耗时: " << sec << " s\n";
    std::cout << "吞吐: " << static_cast<int64_t>(rps) << " req/s\n";
    std::cout << "成功率: " << success_rate << "%\n";
    std::cout << "========================================\n";
}

int main(int argc, char* argv[]) {
    std::string host = "localhost";
    uint16_t port    = 9080;
    int connections   = 10;
    int streams       = 100;
    int rounds        = 5;
    int max_wait      = 120;
    int io_schedulers = 1;

    if (argc > 1) host        = argv[1];
    if (argc > 2) port        = std::atoi(argv[2]);
    if (argc > 3) connections = std::atoi(argv[3]);
    if (argc > 4) streams     = std::atoi(argv[4]);
    if (argc > 5) rounds      = std::atoi(argv[5]);
    if (argc > 6) max_wait    = std::atoi(argv[6]);
    if (argc > 7) io_schedulers = std::max(1, std::atoi(argv[7]));

    int64_t total_requests = static_cast<int64_t>(connections) * streams * rounds;

    std::cout << "========================================\n";
    std::cout << "H2c Mux Client Benchmark\n";
    std::cout << "========================================\n";
    std::cout << "用法: " << argv[0]
              << " [host] [port] [connections] [streams] [rounds] [max_wait] [io_threads]\n";
    std::cout << "目标: " << host << ":" << port << "\n";
    std::cout << "连接数: " << connections << "\n";
    std::cout << "每连接并发流: " << streams << "\n";
    std::cout << "轮次: " << rounds << "\n";
    std::cout << "总请求: " << total_requests << "\n";
    std::cout << "IO 调度器线程: " << io_schedulers << "\n";
    std::cout << "========================================\n\n";

    Runtime rt = RuntimeBuilder().ioSchedulerCount(io_schedulers).computeSchedulerCount(0).build();
    rt.start();

    auto t0 = std::chrono::steady_clock::now();

    std::vector<std::shared_ptr<H2cClient>> client_pool;
    client_pool.reserve(connections);

    for (int i = 0; i < connections; i++) {
        auto client = std::make_shared<H2cClient>(H2cClientBuilder().buildConfig());
        client_pool.push_back(client);
        auto* sched = rt.getNextIOScheduler();
        sched->spawn(runConnection(std::move(client), i, host, port, streams, rounds));
    }

    std::cout << "压测进行中";
    for (int elapsed = 0; ; elapsed++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "." << std::flush;

        if (g.active.load() == 0) break;
        if (max_wait > 0 && elapsed >= max_wait) {
            std::cerr << "\n[warn] wait timeout, active=" << g.active.load()
                      << ", done=" << (g.success.load() + g.fail.load())
                      << "/" << total_requests << "\n";
            break;
        }
    }
    std::cout << "\n";

    auto t1 = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);

    if (g.active.load() > 0) {
        const auto grace_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (g.active.load() > 0 && std::chrono::steady_clock::now() < grace_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    rt.stop();

    printResults(connections, streams, rounds, elapsed);

    return 0;
}
