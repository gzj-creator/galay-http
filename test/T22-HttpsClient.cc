/**
 * @file T22-HttpsClient.cc
 * @brief HTTPS 客户端测试
 */

#include "galay-http/kernel/http/HttpClient.h"
#include "galay-kernel/kernel/Runtime.h"
#include <iostream>
#include <thread>
#include <chrono>

using namespace galay::http;
using namespace galay::kernel;

#ifdef GALAY_HTTP_SSL_ENABLED

Coroutine testHttpsClient() {
    std::cout << "=== HTTPS Client Test ===" << std::endl;

    HttpsClientConfig config;
    config.verify_peer = false;  // 测试时不验证证书

    HttpsClient client(config);

    try {
        // 连接到本地 HTTPS 服务器
        std::cout << "Connecting to https://localhost:8443/..." << std::endl;
        auto connect_result = co_await client.connect("https://localhost:8443/");
        if (!connect_result) {
            std::cerr << "Connect failed: " << connect_result.error().message() << std::endl;
            co_return;
        }
        std::cout << "TCP connection established" << std::endl;

        // SSL 握手
        std::cout << "Performing SSL handshake..." << std::endl;
        while (!client.isHandshakeCompleted()) {
            auto handshake_result = co_await client.handshake();
            if (!handshake_result) {
                auto& err = handshake_result.error();
                if (err.code() == galay::ssl::SslErrorCode::kHandshakeWantRead ||
                    err.code() == galay::ssl::SslErrorCode::kHandshakeWantWrite) {
                    continue;
                }
                std::cerr << "SSL handshake failed: " << err.message() << std::endl;
                co_await client.close();
                co_return;
            }
            break;
        }
        std::cout << "SSL handshake completed" << std::endl;

        // 发送 GET 请求
        std::cout << "Sending GET request..." << std::endl;
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
                std::cerr << "Send failed: " << send_result.error().message() << std::endl;
                co_await client.close();
                co_return;
            }
            if (send_result.value()) break;
        }
        std::cout << "Request sent" << std::endl;

        // 接收响应
        std::cout << "Receiving response..." << std::endl;
        HttpResponse response;
        auto& reader = client.getReader();
        int recv_attempts = 0;
        while (true) {
            recv_attempts++;
            std::cout << "  Recv attempt " << recv_attempts << "..." << std::endl;
            auto recv_result = co_await reader.getResponse(response);
            if (!recv_result) {
                auto& err = recv_result.error();
                std::cerr << "Recv failed (attempt " << recv_attempts << "): "
                         << recv_result.error().message()
                         << " (code: " << static_cast<int>(err.code()) << ")" << std::endl;

                // 如果是连接关闭，检查是否已经收到部分响应
                if (static_cast<int>(response.header().code()) != 0) {
                    std::cout << "Partial response received before connection closed" << std::endl;
                }
                break;
            }
            if (recv_result.value()) {
                std::cout << "  Response complete!" << std::endl;
                break;
            }
            std::cout << "  Need more data..." << std::endl;
        }

        std::cout << "Response received:" << std::endl;
        std::cout << "  Complete: " << (response.isComplete() ? "yes" : "no") << std::endl;
        std::cout << "  Status: " << static_cast<int>(response.header().code()) << std::endl;
        std::cout << "  Body length: " << response.getBodyStr().size() << std::endl;
        std::cout << "  Body: " << response.getBodyStr() << std::endl;

        co_await client.close();
        std::cout << "=== HTTPS Client Test Completed ===" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
    }

    co_return;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "HTTPS Client Test" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Note: Start T21-HttpsServer first!" << std::endl;
    std::cout << std::endl;

    // 创建运行时
    Runtime rt(LoadBalanceStrategy::ROUND_ROBIN, 1, 0);
    rt.start();

    auto* scheduler = rt.getNextIOScheduler();
    if (!scheduler) {
        std::cerr << "No IO scheduler available" << std::endl;
        return 1;
    }

    scheduler->spawn(testHttpsClient());

    // 等待测试完成
    std::this_thread::sleep_for(std::chrono::seconds(5));

    rt.stop();

    return 0;
}

#else

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "HTTPS Client Test" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "SSL support is not enabled." << std::endl;
    std::cout << "Rebuild with -DGALAY_HTTP_ENABLE_SSL=ON" << std::endl;
    return 0;
}

#endif
