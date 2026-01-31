/**
 * @file T23-HttpsStressTest.cc
 * @brief HTTPS 压力测试 - 使用 keep-alive 连接复用
 */

#include "galay-http/kernel/http/HttpClient.h"
#include "galay-kernel/kernel/Runtime.h"
#include <iostream>
#include <atomic>
#include <chrono>
#include <vector>

using namespace galay::http;
using namespace galay::kernel;

#ifdef GALAY_HTTP_SSL_ENABLED

std::atomic<int> g_success{0};
std::atomic<int> g_fail{0};
std::atomic<int> g_completed{0};

// 单连接多请求 (keep-alive)
Coroutine keepAliveRequests(int conn_id, int requests_per_conn) {
    HttpsClientConfig config;
    config.verify_peer = false;

    HttpsClient client(config);

    try {
        // 连接
        auto connect_result = co_await client.connect("https://localhost:8443/");
        if (!connect_result) {
            g_fail += requests_per_conn;
            g_completed += requests_per_conn;
            co_return;
        }

        // SSL 握手
        while (!client.isHandshakeCompleted()) {
            auto handshake_result = co_await client.handshake();
            if (!handshake_result) {
                auto& err = handshake_result.error();
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

        // 在同一连接上发送多个请求
        for (int i = 0; i < requests_per_conn; i++) {
            // 构建请求
            HttpRequest request;
            HttpRequestHeader header;
            header.method() = HttpMethod::GET;
            header.uri() = "/";
            header.version() = HttpVersion::HttpVersion_1_1;
            header.headerPairs().addHeaderPair("Host", "localhost");
            header.headerPairs().addHeaderPair("Connection", "keep-alive");
            request.setHeader(std::move(header));

            // 发送请求
            bool send_ok = false;
            while (true) {
                auto send_result = co_await writer.sendRequest(request);
                if (!send_result) {
                    break;
                }
                if (send_result.value()) {
                    send_ok = true;
                    break;
                }
            }

            if (!send_ok) {
                g_fail++;
                g_completed++;
                continue;
            }

            // 接收响应
            HttpResponse response;
            bool recv_ok = false;
            while (true) {
                auto recv_result = co_await reader.getResponse(response);
                if (!recv_result) {
                    break;
                }
                if (recv_result.value()) {
                    recv_ok = true;
                    break;
                }
            }

            if (recv_ok && static_cast<int>(response.header().code()) == 200) {
                g_success++;
            } else {
                g_fail++;
            }
            g_completed++;

            // 重置 response 以便下次使用
            response = HttpResponse();
        }

        co_await client.close();

    } catch (...) {
        g_fail++;
        g_completed++;
    }

    co_return;
}

void runKeepAliveTest(Runtime& rt, int total_requests, int connections, const std::string& test_name) {
    g_success = 0;
    g_fail = 0;
    g_completed = 0;

    int requests_per_conn = total_requests / connections;

    std::cout << "\n=== " << test_name << " ===" << std::endl;
    std::cout << "总请求: " << total_requests << ", 连接数: " << connections
              << ", 每连接请求: " << requests_per_conn << std::endl;

    auto start = std::chrono::steady_clock::now();

    // 启动所有连接
    for (int i = 0; i < connections; i++) {
        auto* scheduler = rt.getNextIOScheduler();
        if (scheduler) {
            scheduler->spawn(keepAliveRequests(i, requests_per_conn));
        }
    }

    // 等待所有请求完成
    while (g_completed < total_requests) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto end = std::chrono::steady_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    double qps = (duration_ms > 0) ? (g_success.load() * 1000.0 / duration_ms) : 0;
    double success_rate = (total_requests > 0) ? (g_success.load() * 100.0 / total_requests) : 0;

    std::cout << "结果: 成功=" << g_success << ", 失败=" << g_fail << std::endl;
    std::cout << "成功率: " << success_rate << "%" << std::endl;
    std::cout << "耗时: " << duration_ms << "ms" << std::endl;
    std::cout << "QPS: " << qps << std::endl;
}

int main() {
    std::cout << "==========================================" << std::endl;
    std::cout << "HTTPS 压力测试 (Keep-Alive 连接复用)" << std::endl;
    std::cout << "==========================================" << std::endl;
    std::cout << "请确保 T21-HttpsServer 已在 8443 端口运行!" << std::endl;

    Runtime rt(LoadBalanceStrategy::ROUND_ROBIN, 4, 0);
    rt.start();

    // 测试1: 单连接 100 请求
    runKeepAliveTest(rt, 100, 1, "测试1: 单连接 100请求");

    // 测试2: 10连接 各100请求
    runKeepAliveTest(rt, 1000, 10, "测试2: 10连接 各100请求");

    // 测试3: 20连接 各100请求
    runKeepAliveTest(rt, 2000, 20, "测试3: 20连接 各100请求");

    // 测试4: 50连接 各100请求
    runKeepAliveTest(rt, 5000, 50, "测试4: 50连接 各100请求");

    // 测试5: 100连接 各100请求
    runKeepAliveTest(rt, 10000, 100, "测试5: 100连接 各100请求");

    rt.stop();

    std::cout << "\n==========================================" << std::endl;
    std::cout << "压测完成" << std::endl;
    std::cout << "==========================================" << std::endl;

    return 0;
}

#else

int main() {
    std::cout << "SSL support is not enabled." << std::endl;
    std::cout << "Rebuild with -DGALAY_HTTP_ENABLE_SSL=ON" << std::endl;
    return 0;
}

#endif
