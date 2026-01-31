/**
 * @file B7-HttpsBenchmark.cc
 * @brief HTTPS 服务器/客户端压力测试
 * @details 测试 HTTPS Keep-Alive 连接复用性能
 */

#include "galay-http/kernel/http/HttpClient.h"
#include "galay-kernel/kernel/Runtime.h"
#include <iostream>
#include <atomic>
#include <chrono>
#include <iomanip>

using namespace galay::http;
using namespace galay::kernel;

#ifdef GALAY_HTTP_SSL_ENABLED

std::atomic<int64_t> g_success{0};
std::atomic<int64_t> g_fail{0};
std::atomic<int64_t> g_completed{0};
std::atomic<int64_t> g_bytes_sent{0};
std::atomic<int64_t> g_bytes_recv{0};

// Keep-Alive 连接压测
Coroutine keepAliveWorker(int worker_id, int requests_per_conn, const std::string& host, int port) {
    HttpsClientConfig config;
    config.verify_peer = false;

    HttpsClient client(config);

    try {
        std::string url = "https://" + host + ":" + std::to_string(port) + "/";
        auto connect_result = co_await client.connect(url);
        if (!connect_result) {
            g_fail += requests_per_conn;
            g_completed += requests_per_conn;
            co_return;
        }

        while (!client.isHandshakeCompleted()) {
            auto hs = co_await client.handshake();
            if (!hs) {
                auto& err = hs.error();
                if (err.code() == galay::ssl::SslErrorCode::kHandshakeWantRead ||
                    err.code() == galay::ssl::SslErrorCode::kHandshakeWantWrite) {
                    continue;
                }
                g_fail += requests_per_conn;
                g_completed += requests_per_conn;
                co_await client.close();
                co_return;
            }
            break;
        }

        auto& writer = client.getWriter();
        auto& reader = client.getReader();

        for (int i = 0; i < requests_per_conn; i++) {
            HttpRequest request;
            HttpRequestHeader header;
            header.method() = HttpMethod::GET;
            header.uri() = "/";
            header.version() = HttpVersion::HttpVersion_1_1;
            header.headerPairs().addHeaderPair("Host", host);
            header.headerPairs().addHeaderPair("Connection", "keep-alive");
            request.setHeader(std::move(header));

            bool send_ok = false;
            while (true) {
                auto r = co_await writer.sendRequest(request);
                if (!r) break;
                if (r.value()) { send_ok = true; break; }
            }

            if (!send_ok) {
                g_fail++;
                g_completed++;
                continue;
            }
            g_bytes_sent += 100;  // 估算请求大小

            HttpResponse response;
            bool recv_ok = false;
            while (true) {
                auto r = co_await reader.getResponse(response);
                if (!r) break;
                if (r.value()) { recv_ok = true; break; }
            }

            if (recv_ok && static_cast<int>(response.header().code()) == 200) {
                g_success++;
                g_bytes_recv += response.getBodyStr().size();
            } else {
                g_fail++;
            }
            g_completed++;

            response = HttpResponse();
        }

        co_await client.close();

    } catch (...) {
        g_fail++;
        g_completed++;
    }

    co_return;
}

void runBenchmark(Runtime& rt, int total_requests, int connections,
                  const std::string& host, int port, const std::string& name) {
    g_success = 0;
    g_fail = 0;
    g_completed = 0;
    g_bytes_sent = 0;
    g_bytes_recv = 0;

    int requests_per_conn = total_requests / connections;

    std::cout << "\n=== " << name << " ===\n";
    std::cout << "请求数: " << total_requests << ", 连接数: " << connections
              << ", 每连接: " << requests_per_conn << "\n";

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < connections; i++) {
        auto* scheduler = rt.getNextIOScheduler();
        if (scheduler) {
            scheduler->spawn(keepAliveWorker(i, requests_per_conn, host, port));
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

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "结果: 成功=" << g_success << ", 失败=" << g_fail << "\n";
    std::cout << "成功率: " << success_rate << "%\n";
    std::cout << "耗时: " << duration_ms << "ms\n";
    std::cout << "QPS: " << qps << "\n";
    std::cout << "吞吐量: " << throughput_mbps << " MB/s\n";
}

int main(int argc, char* argv[]) {
    std::string host = "localhost";
    int port = 8443;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = std::atoi(argv[2]);

    std::cout << "==========================================\n";
    std::cout << "HTTPS Benchmark (Keep-Alive)\n";
    std::cout << "==========================================\n";
    std::cout << "目标: " << host << ":" << port << "\n";
    std::cout << "请确保 HTTPS 服务器已启动!\n";

    Runtime rt(LoadBalanceStrategy::ROUND_ROBIN, 4, 0);
    rt.start();

    runBenchmark(rt, 100, 1, host, port, "单连接 100请求");
    runBenchmark(rt, 1000, 10, host, port, "10连接 各100请求");
    runBenchmark(rt, 2000, 20, host, port, "20连接 各100请求");
    runBenchmark(rt, 5000, 50, host, port, "50连接 各100请求");
    runBenchmark(rt, 10000, 100, host, port, "100连接 各100请求");

    rt.stop();

    std::cout << "\n==========================================\n";
    std::cout << "压测完成\n";
    std::cout << "==========================================\n";

    return 0;
}

#else

int main() {
    std::cout << "SSL support is not enabled.\n";
    std::cout << "Rebuild with -DGALAY_HTTP_ENABLE_SSL=ON\n";
    return 0;
}

#endif
