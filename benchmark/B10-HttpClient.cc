/**
 * @file B10-HttpClientBenchmark.cc
 * @brief HTTP 客户端压力测试
 * @details 测试 HttpClient 的并发请求性能和连接复用能力
 */

#include "galay-http/kernel/http/HttpClient.h"
#include "galay-kernel/kernel/Runtime.h"
#include <iostream>
#include <atomic>
#include <chrono>
#include <iomanip>

using namespace galay::http;
using namespace galay::kernel;

std::atomic<int64_t> g_success{0};
std::atomic<int64_t> g_fail{0};
std::atomic<int64_t> g_completed{0};
std::atomic<int64_t> g_bytes_sent{0};
std::atomic<int64_t> g_bytes_recv{0};
std::atomic<int64_t> g_connect_time_us{0};
std::atomic<int64_t> g_request_time_us{0};

/**
 * @brief 单个请求的工作协程（短连接模式）
 */
Coroutine shortConnectionWorker(int worker_id, const std::string& host, int port, const std::string& path) {
    HttpClient client;

    try {
        std::string url = "http://" + host + ":" + std::to_string(port) + path;

        auto connect_start = std::chrono::steady_clock::now();
        auto connect_result = co_await client.connect(url);
        auto connect_end = std::chrono::steady_clock::now();

        if (!connect_result) {
            g_fail++;
            g_completed++;
            co_return;
        }

        g_connect_time_us += std::chrono::duration_cast<std::chrono::microseconds>(connect_end - connect_start).count();

        auto request_start = std::chrono::steady_clock::now();
        auto session = client.getSession();
        // 使用 HttpClient 的便捷方法发送 GET 请求
        while (true) {
            auto result = co_await session.get(client.url().path, {
                {"Host", host},
                {"Connection", "close"}
            });

            if (!result) {
                g_fail++;
                g_completed++;
                co_return;
            }

            if (!result.value()) {
                continue;
            }

            auto response = result.value().value();
            auto request_end = std::chrono::steady_clock::now();

            g_request_time_us += std::chrono::duration_cast<std::chrono::microseconds>(request_end - request_start).count();

            if (static_cast<int>(response.header().code()) == 200) {
                g_success++;
                g_bytes_recv += response.getBodyStr().size();
                g_bytes_sent += 100;  // 估算请求大小
            } else {
                g_fail++;
            }
            g_completed++;
            break;
        }

        co_await client.close();

    } catch (...) {
        g_fail++;
        g_completed++;
    }

    co_return;
}

/**
 * @brief Keep-Alive 长连接模式工作协程
 */
Coroutine keepAliveWorker(int worker_id, int requests_per_conn, const std::string& host, int port, const std::string& path) {
    HttpClient client;

    try {
        std::string url = "http://" + host + ":" + std::to_string(port) + path;

        auto connect_start = std::chrono::steady_clock::now();
        auto connect_result = co_await client.connect(url);
        auto connect_end = std::chrono::steady_clock::now();

        if (!connect_result) {
            g_fail += requests_per_conn;
            g_completed += requests_per_conn;
            co_return;
        }

        g_connect_time_us += std::chrono::duration_cast<std::chrono::microseconds>(connect_end - connect_start).count();
        auto session = client.getSession();
        for (int i = 0; i < requests_per_conn; i++) {
            auto request_start = std::chrono::steady_clock::now();

            while (true) {
                auto result = co_await session.get(client.url().path, {
                    {"Host", host},
                    {"Connection", "keep-alive"}
                });

                if (!result) {
                    g_fail++;
                    g_completed++;
                    break;
                }

                if (!result.value()) {
                    continue;
                }

                auto response = result.value().value();
                auto request_end = std::chrono::steady_clock::now();

                g_request_time_us += std::chrono::duration_cast<std::chrono::microseconds>(request_end - request_start).count();

                if (static_cast<int>(response.header().code()) == 200) {
                    g_success++;
                    g_bytes_recv += response.getBodyStr().size();
                    g_bytes_sent += 100;  // 估算请求大小
                } else {
                    g_fail++;
                }
                g_completed++;
                break;
            }
        }

        co_await client.close();

    } catch (...) {
        g_fail += requests_per_conn;
        g_completed += requests_per_conn;
    }

    co_return;
}

/**
 * @brief 运行短连接压测
 */
void runShortConnectionBenchmark(Runtime& rt, int total_requests, int concurrency,
                                  const std::string& host, int port, const std::string& path,
                                  const std::string& name) {
    g_success = 0;
    g_fail = 0;
    g_completed = 0;
    g_bytes_sent = 0;
    g_bytes_recv = 0;
    g_connect_time_us = 0;
    g_request_time_us = 0;

    std::cout << "\n=== " << name << " ===\n";
    std::cout << "总请求数: " << total_requests << ", 并发数: " << concurrency << "\n";

    auto start = std::chrono::steady_clock::now();

    int spawned = 0;
    while (spawned < total_requests) {
        auto* scheduler = rt.getNextIOScheduler();
        if (scheduler) {
            scheduler->spawn(shortConnectionWorker(spawned, host, port, path));
            spawned++;
        }
    }

    while (g_completed < total_requests) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto end = std::chrono::steady_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    double qps = (duration_ms > 0) ? (g_success.load() * 1000.0 / duration_ms) : 0;
    double success_rate = (total_requests > 0) ? (g_success.load() * 100.0 / total_requests) : 0;
    double throughput_mb = (g_bytes_recv.load() + g_bytes_sent.load()) / 1024.0 / 1024.0;
    double throughput_mbps = (duration_ms > 0) ? (throughput_mb * 1000.0 / duration_ms) : 0;
    double avg_connect_ms = (g_success.load() > 0) ? (g_connect_time_us.load() / 1000.0 / g_success.load()) : 0;
    double avg_request_ms = (g_success.load() > 0) ? (g_request_time_us.load() / 1000.0 / g_success.load()) : 0;

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "结果: 成功=" << g_success << ", 失败=" << g_fail << "\n";
    std::cout << "成功率: " << success_rate << "%\n";
    std::cout << "耗时: " << duration_ms << "ms\n";
    std::cout << "QPS: " << qps << "\n";
    std::cout << "吞吐量: " << throughput_mbps << " MB/s\n";
    std::cout << "平均连接时间: " << avg_connect_ms << "ms\n";
    std::cout << "平均请求时间: " << avg_request_ms << "ms\n";
}

/**
 * @brief 运行长连接压测
 */
void runKeepAliveBenchmark(Runtime& rt, int total_requests, int connections,
                           const std::string& host, int port, const std::string& path,
                           const std::string& name) {
    g_success = 0;
    g_fail = 0;
    g_completed = 0;
    g_bytes_sent = 0;
    g_bytes_recv = 0;
    g_connect_time_us = 0;
    g_request_time_us = 0;

    int requests_per_conn = total_requests / connections;

    std::cout << "\n=== " << name << " ===\n";
    std::cout << "总请求数: " << total_requests << ", 连接数: " << connections
              << ", 每连接请求数: " << requests_per_conn << "\n";

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < connections; i++) {
        auto* scheduler = rt.getNextIOScheduler();
        if (scheduler) {
            scheduler->spawn(keepAliveWorker(i, requests_per_conn, host, port, path));
        }
    }

    while (g_completed < total_requests) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto end = std::chrono::steady_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    double qps = (duration_ms > 0) ? (g_success.load() * 1000.0 / duration_ms) : 0;
    double success_rate = (total_requests > 0) ? (g_success.load() * 100.0 / total_requests) : 0;
    double throughput_mb = (g_bytes_recv.load() + g_bytes_sent.load()) / 1024.0 / 1024.0;
    double throughput_mbps = (duration_ms > 0) ? (throughput_mb * 1000.0 / duration_ms) : 0;
    double avg_connect_ms = (connections > 0) ? (g_connect_time_us.load() / 1000.0 / connections) : 0;
    double avg_request_ms = (g_success.load() > 0) ? (g_request_time_us.load() / 1000.0 / g_success.load()) : 0;

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "结果: 成功=" << g_success << ", 失败=" << g_fail << "\n";
    std::cout << "成功率: " << success_rate << "%\n";
    std::cout << "耗时: " << duration_ms << "ms\n";
    std::cout << "QPS: " << qps << "\n";
    std::cout << "吞吐量: " << throughput_mbps << " MB/s\n";
    std::cout << "平均连接时间: " << avg_connect_ms << "ms\n";
    std::cout << "平均请求时间: " << avg_request_ms << "ms\n";
}

int main(int argc, char* argv[]) {
    std::string host = "localhost";
    int port = 8080;
    std::string path = "/";

    if (argc > 1) host = argv[1];
    if (argc > 2) port = std::atoi(argv[2]);
    if (argc > 3) path = argv[3];

    std::cout << "==========================================\n";
    std::cout << "HTTP Client Benchmark\n";
    std::cout << "==========================================\n";
    std::cout << "目标: http://" << host << ":" << port << path << "\n";
    std::cout << "请确保 HTTP 服务器已启动!\n";
    std::cout << "建议使用: ./E1-EchoServer " << port << "\n";
    std::cout << "==========================================\n";

    Runtime rt(4, 0);
    rt.start();

    std::cout << "\n【短连接模式测试】\n";
    std::cout << "每个请求建立新连接，测试连接建立和请求处理性能\n";
    runShortConnectionBenchmark(rt, 100, 10, host, port, path, "短连接 100请求 10并发");
    runShortConnectionBenchmark(rt, 500, 50, host, port, path, "短连接 500请求 50并发");
    runShortConnectionBenchmark(rt, 1000, 100, host, port, path, "短连接 1000请求 100并发");

    std::cout << "\n【Keep-Alive 长连接模式测试】\n";
    std::cout << "复用连接发送多个请求，测试连接复用性能\n";
    runKeepAliveBenchmark(rt, 100, 1, host, port, path, "长连接 单连接100请求");
    runKeepAliveBenchmark(rt, 1000, 10, host, port, path, "长连接 10连接各100请求");
    runKeepAliveBenchmark(rt, 2000, 20, host, port, path, "长连接 20连接各100请求");
    runKeepAliveBenchmark(rt, 5000, 50, host, port, path, "长连接 50连接各100请求");
    runKeepAliveBenchmark(rt, 10000, 100, host, port, path, "长连接 100连接各100请求");

    rt.stop();

    std::cout << "\n==========================================\n";
    std::cout << "压测完成\n";
    std::cout << "==========================================\n";

    return 0;
}
