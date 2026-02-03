/**
 * @file E13-H2Client.cc
 * @brief h2 (HTTP/2 over TLS) 客户端示例
 * @details 演示如何通过 TLS ALPN 协商使用 HTTP/2
 *
 * 测试方法:
 *   # 使用支持 h2 的服务器（如 nghttp2）
 *   nghttpd -v 8443 cert/server.key cert/server.crt
 *
 *   # 然后运行客户端
 *   ./E13-H2Client localhost 8443
 *
 *   # 或者测试公网 HTTP/2 服务器
 *   ./E13-H2Client www.google.com 443
 */

#include "galay-http/kernel/http2/H2Client.h"
#include "galay-kernel/kernel/Runtime.h"
#include <iostream>

using namespace galay::http2;
using namespace galay::kernel;

Coroutine runClient(const std::string& host, uint16_t port) {
    H2ClientConfig config;
    config.verify_peer = false;  // 测试时不验证证书

    H2Client client(config);

    std::cout << "Connecting to " << host << ":" << port << " (TLS)...\n";

    // 连接（包含 TLS 握手和 HTTP/2 协商）
    while (true) {
        auto result = co_await client.connect(host, port);
        if (!result) {
            std::cerr << "Connect failed: " << http2ErrorCodeToString(result.error()) << "\n";
            co_return;
        }
        if (result.value().has_value()) {
            if (*result.value()) {
                std::cout << "Connected! ALPN: " << client.getALPNProtocol() << "\n\n";
                break;
            } else {
                std::cerr << "Connection failed\n";
                co_return;
            }
        }
    }

    // 发送 GET / 请求
    {
        std::cout << "=== GET / ===\n";
        while (true) {
            auto result = co_await client.get("/");
            if (!result) {
                std::cerr << "Request failed: " << http2ErrorCodeToString(result.error()) << "\n";
                break;
            }
            if (result.value()) {
                auto& response = *result.value();
                std::cout << "Status: " << response.status << "\n";
                std::cout << "Headers:\n";
                for (const auto& h : response.headers) {
                    std::cout << "  " << h.name << ": " << h.value << "\n";
                }
                std::cout << "Body (" << response.body.size() << " bytes)\n";
                if (response.body.size() < 500) {
                    std::cout << response.body << "\n";
                }
                std::cout << "\n";
                break;
            }
        }
    }

    // 发送 GET /robots.txt 请求
    {
        std::cout << "=== GET /robots.txt ===\n";
        while (true) {
            auto result = co_await client.get("/robots.txt");
            if (!result) {
                std::cerr << "Request failed: " << http2ErrorCodeToString(result.error()) << "\n";
                break;
            }
            if (result.value()) {
                auto& response = *result.value();
                std::cout << "Status: " << response.status << "\n";
                std::cout << "Body (" << response.body.size() << " bytes)\n";
                if (response.body.size() < 500) {
                    std::cout << response.body << "\n";
                }
                std::cout << "\n";
                break;
            }
        }
    }

    // 关闭连接
    co_await client.close();
    std::cout << "Connection closed.\n";

    co_return;
}

int main(int argc, char* argv[]) {
    std::string host = "localhost";
    uint16_t port = 8443;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = std::atoi(argv[2]);

    std::cout << "========================================\n";
    std::cout << "H2 (HTTP/2 over TLS) Client Example\n";
    std::cout << "========================================\n";

    try {
        Runtime runtime(1, 0);
        runtime.start();

        auto* scheduler = runtime.getNextIOScheduler();
        scheduler->spawn(runClient(host, port));

        std::this_thread::sleep_for(std::chrono::seconds(10));

        runtime.stop();

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
