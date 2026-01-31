/**
 * @file T24-SimpleHttpsTest.cc
 * @brief 简单的 HTTPS 测试 - 用于调试
 */

#include "galay-http/kernel/http/HttpClient.h"
#include "galay-kernel/kernel/Runtime.h"
#include <iostream>
#include <atomic>

using namespace galay::http;
using namespace galay::kernel;

#ifdef GALAY_HTTP_SSL_ENABLED

std::atomic<int> g_success{0};
std::atomic<int> g_fail{0};

Coroutine singleRequest(int id) {
    std::cout << "[Request " << id << "] Starting..." << std::endl;

    HttpsClientConfig config;
    config.verify_peer = false;

    HttpsClient client(config);

    try {
        // 连接
        std::cout << "[Request " << id << "] Connecting..." << std::endl;
        auto connect_result = co_await client.connect("https://localhost:8443/");
        if (!connect_result) {
            std::cerr << "[Request " << id << "] Connect failed: " << connect_result.error().message() << std::endl;
            g_fail++;
            co_return;
        }
        std::cout << "[Request " << id << "] Connected" << std::endl;

        // SSL 握手
        std::cout << "[Request " << id << "] Handshaking..." << std::endl;
        while (!client.isHandshakeCompleted()) {
            auto handshake_result = co_await client.handshake();
            if (!handshake_result) {
                auto& err = handshake_result.error();
                if (err.code() == galay::ssl::SslErrorCode::kHandshakeWantRead ||
                    err.code() == galay::ssl::SslErrorCode::kHandshakeWantWrite) {
                    continue;
                }
                std::cerr << "[Request " << id << "] Handshake failed: " << err.message() << std::endl;
                g_fail++;
                co_await client.close();
                co_return;
            }
            break;
        }
        std::cout << "[Request " << id << "] Handshake completed" << std::endl;

        // 发送请求
        std::cout << "[Request " << id << "] Sending request..." << std::endl;
        HttpRequest request;
        HttpRequestHeader header;
        header.method() = HttpMethod::GET;
        header.uri() = "/";
        header.version() = HttpVersion::HttpVersion_1_1;
        header.headerPairs().addHeaderPair("Host", "localhost");
        header.headerPairs().addHeaderPair("Connection", "close");
        request.setHeader(std::move(header));

        auto& writer = client.getWriter();
        while (true) {
            auto send_result = co_await writer.sendRequest(request);
            if (!send_result) {
                std::cerr << "[Request " << id << "] Send failed: " << send_result.error().message() << std::endl;
                g_fail++;
                co_await client.close();
                co_return;
            }
            if (send_result.value()) break;
        }
        std::cout << "[Request " << id << "] Request sent" << std::endl;

        // 接收响应
        std::cout << "[Request " << id << "] Receiving response..." << std::endl;
        HttpResponse response;
        auto& reader = client.getReader();
        while (true) {
            auto recv_result = co_await reader.getResponse(response);
            if (!recv_result) {
                std::cerr << "[Request " << id << "] Recv failed: " << recv_result.error().message() << std::endl;
                g_fail++;
                co_await client.close();
                co_return;
            }
            if (recv_result.value()) break;
        }

        std::cout << "[Request " << id << "] Response received: " << static_cast<int>(response.header().code()) << std::endl;

        // 验证响应
        if (static_cast<int>(response.header().code()) == 200) {
            g_success++;
            std::cout << "[Request " << id << "] SUCCESS" << std::endl;
        } else {
            g_fail++;
            std::cout << "[Request " << id << "] FAILED - wrong status code" << std::endl;
        }

        co_await client.close();

    } catch (const std::exception& e) {
        std::cerr << "[Request " << id << "] Exception: " << e.what() << std::endl;
        g_fail++;
    }

    co_return;
}

int main() {
    std::cout << "==========================================" << std::endl;
    std::cout << "简单 HTTPS 测试 (调试用)" << std::endl;
    std::cout << "==========================================" << std::endl;

    // 创建运行时
    Runtime rt(LoadBalanceStrategy::ROUND_ROBIN, 2, 0);
    rt.start();

    // 发送 20 个顺序请求
    for (int i = 0; i < 20; i++) {
        auto* scheduler = rt.getNextIOScheduler();
        if (scheduler) {
            scheduler->spawn(singleRequest(i));
        }
        // 等待每个请求完成
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // 等待所有请求完成
    std::this_thread::sleep_for(std::chrono::seconds(5));

    rt.stop();

    std::cout << "\n==========================================" << std::endl;
    std::cout << "测试完成" << std::endl;
    std::cout << "成功: " << g_success << ", 失败: " << g_fail << std::endl;
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
