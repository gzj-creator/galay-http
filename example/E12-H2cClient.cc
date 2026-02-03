/**
 * @file E12-H2cClient.cc
 * @brief h2c (HTTP/2 over cleartext) 客户端示例
 * @details 演示如何通过 HTTP/1.1 Upgrade 机制升级到 HTTP/2
 *
 * 测试方法:
 *   # 使用支持 h2c 升级的服务器（如 nghttp2）
 *   nghttpd -v 9080 --no-tls
 *
 *   # 然后运行客户端
 *   ./E12-H2cClient localhost 9080
 */

#include "galay-http/kernel/http2/H2cClient.h"
#include "galay-kernel/kernel/Runtime.h"
#include <iostream>

using namespace galay::http2;
using namespace galay::kernel;

Coroutine runClient(const std::string& host, uint16_t port) {
    H2cClient client;

    std::cout << "Connecting to " << host << ":" << port << "...\n";

    // 连接
    auto connect_result = co_await client.connect(host, port);
    if (!connect_result) {
        std::cerr << "Connect failed: " << connect_result.error().message() << "\n";
        co_return;
    }
    std::cout << "Connected!\n";

    // 升级到 HTTP/2
    std::cout << "Upgrading to HTTP/2...\n";
    while (true) {
        auto result = co_await client.upgrade("/");
        if (!result) {
            std::cerr << "Upgrade failed: " << http2ErrorCodeToString(result.error()) << "\n";
            co_await client.close();
            co_return;
        }
        if (result.value().has_value()) {
            if (*result.value()) {
                std::cout << "Upgraded to HTTP/2!\n\n";
                break;
            } else {
                std::cerr << "Server does not support h2c upgrade\n";
                co_await client.close();
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
                std::cout << "Body (" << response.body.size() << " bytes)\n\n";
                break;
            }
        }
    }

    // 发送 GET /json 请求
    {
        std::cout << "=== GET /json ===\n";
        while (true) {
            auto result = co_await client.get("/json");
            if (!result) {
                std::cerr << "Request failed: " << http2ErrorCodeToString(result.error()) << "\n";
                break;
            }
            if (result.value()) {
                auto& response = *result.value();
                std::cout << "Status: " << response.status << "\n";
                std::cout << "Body: " << response.body << "\n\n";
                break;
            }
        }
    }

    // 发送 POST /echo 请求
    {
        std::cout << "=== POST /echo ===\n";
        while (true) {
            auto result = co_await client.post("/echo", "Hello from H2cClient!", "text/plain");
            if (!result) {
                std::cerr << "Request failed: " << http2ErrorCodeToString(result.error()) << "\n";
                break;
            }
            if (result.value()) {
                auto& response = *result.value();
                std::cout << "Status: " << response.status << "\n";
                std::cout << "Body: " << response.body << "\n\n";
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
    uint16_t port = 9080;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = std::atoi(argv[2]);

    std::cout << "========================================\n";
    std::cout << "H2c (HTTP/2 Cleartext) Client Example\n";
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
