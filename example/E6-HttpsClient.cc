/**
 * @file E6-HttpsClient.cc
 * @brief HTTPS 客户端示例
 * @details 演示如何使用 HttpsClient 发送 HTTPS 请求，支持 Keep-Alive 连接复用
 */

#include "galay-http/kernel/http/HttpClient.h"
#include "galay-kernel/kernel/Runtime.h"
#include <iostream>

using namespace galay::http;
using namespace galay::kernel;

#ifdef GALAY_HTTP_SSL_ENABLED

Coroutine httpsClientExample(const std::string& url) {
    std::cout << "Connecting to " << url << "...\n";

    HttpsClientConfig config;
    config.verify_peer = false;  // 测试时不验证证书

    HttpsClient client(config);

    try {
        // 连接
        auto connect_result = co_await client.connect(url);
        if (!connect_result) {
            std::cerr << "Connect failed: " << connect_result.error().message() << "\n";
            co_return;
        }
        std::cout << "TCP connection established\n";

        // SSL 握手
        while (!client.isHandshakeCompleted()) {
            auto handshake_result = co_await client.handshake();
            if (!handshake_result) {
                auto& err = handshake_result.error();
                if (err.code() == galay::ssl::SslErrorCode::kHandshakeWantRead ||
                    err.code() == galay::ssl::SslErrorCode::kHandshakeWantWrite) {
                    continue;
                }
                std::cerr << "Handshake failed: " << err.message() << "\n";
                co_await client.close();
                co_return;
            }
            break;
        }
        std::cout << "SSL handshake completed\n";

        auto& writer = client.getWriter();
        auto& reader = client.getReader();

        // 发送多个请求 (Keep-Alive)
        for (int i = 1; i <= 3; i++) {
            std::cout << "\n--- Request " << i << " ---\n";

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
            while (true) {
                auto send_result = co_await writer.sendRequest(request);
                if (!send_result) {
                    std::cerr << "Send failed: " << send_result.error().message() << "\n";
                    co_await client.close();
                    co_return;
                }
                if (send_result.value()) break;
            }
            std::cout << "Request sent\n";

            // 接收响应
            HttpResponse response;
            while (true) {
                auto recv_result = co_await reader.getResponse(response);
                if (!recv_result) {
                    std::cerr << "Recv failed: " << recv_result.error().message() << "\n";
                    co_await client.close();
                    co_return;
                }
                if (recv_result.value()) break;
            }

            std::cout << "Response: " << static_cast<int>(response.header().code()) << "\n";
            std::cout << "Body length: " << response.getBodyStr().size() << " bytes\n";

            // 重置 response
            response = HttpResponse();
        }

        co_await client.close();
        std::cout << "\nConnection closed\n";

    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
    }

    co_return;
}

int main(int argc, char* argv[]) {
    std::string url = "https://localhost:8443/";
    if (argc > 1) url = argv[1];

    std::cout << "========================================\n";
    std::cout << "HTTPS Client Example\n";
    std::cout << "========================================\n";

    Runtime rt(LoadBalanceStrategy::ROUND_ROBIN, 1, 0);
    rt.start();

    auto* scheduler = rt.getNextIOScheduler();
    if (!scheduler) {
        std::cerr << "No IO scheduler available\n";
        return 1;
    }

    scheduler->spawn(httpsClientExample(url));

    std::this_thread::sleep_for(std::chrono::seconds(5));

    rt.stop();

    return 0;
}

#else

int main() {
    std::cout << "SSL support is not enabled.\n";
    std::cout << "Rebuild with -DGALAY_HTTP_ENABLE_SSL=ON\n";
    return 0;
}

#endif
