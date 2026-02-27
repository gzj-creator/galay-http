/**
 * @file E10-H2cEchoClient.cc
 * @brief h2c (HTTP/2 over cleartext) Echo 客户端示例
 * @details 通过 HTTP/1.1 Upgrade 升级后，使用 StreamManager 发送 Echo 请求
 *
 * 测试方法:
 *   # 先启动 Echo Server
 *   ./E9-H2cEchoServer 8080
 *
 *   # 然后运行客户端
 *   ./E10-H2cEchoClient localhost 8080
 */

#include "galay-http/kernel/http2/H2cClient.h"
#include "galay-kernel/kernel/Runtime.h"
#include <iostream>

using namespace galay::http2;
using namespace galay::kernel;

Coroutine runClient(const std::string& host, uint16_t port) {
    H2cClient client(H2cClientBuilder().build());

    std::cout << "Connecting to " << host << ":" << port << "...\n";

    // 连接
    auto connect_result = co_await client.connect(host, port);
    if (!connect_result) {
        std::cerr << "Connect failed: " << connect_result.error().message() << "\n";
        co_return;
    }
    std::cout << "Connected!\n";

    // 升级到 HTTP/2（内部启动 StreamManager）
    std::cout << "Upgrading to HTTP/2...\n";
    co_await client.upgrade("/").wait();
    if (!client.isUpgraded()) {
        std::cerr << "Upgrade failed\n";
        co_return;
    }
    std::cout << "Upgraded to HTTP/2!\n\n";

    auto* mgr = client.getConn()->streamManager();

    // 发送 POST /echo
    std::string body = "Hello from H2cEchoClient!";
    auto stream = mgr->allocateStream();

    std::cout << "=== POST /echo ===\n";
    stream->sendHeaders(
        Http2Headers().method("POST").scheme("http")
            .authority(host + ":" + std::to_string(port)).path("/echo")
            .contentType("text/plain").contentLength(body.size()),
        false, true);
    stream->sendData(body, true);

    co_await stream->readResponse().wait();
    auto& response = stream->response();

    std::cout << "Status: " << response.status << "\n";
    std::cout << "Body: " << response.body << "\n\n";

    // 优雅关闭
    co_await client.shutdown().wait();
    std::cout << "Connection closed.\n";

    co_return;
}

int main(int argc, char* argv[]) {
    galay::http::HttpLogger::console();  // DEBUG 日志输出到终端
    std::string host = "localhost";
    uint16_t port = 8080;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = std::atoi(argv[2]);

    std::cout << "========================================\n";
    std::cout << "H2c (HTTP/2 Cleartext) Echo Client Example\n";
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
