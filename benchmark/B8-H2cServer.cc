/**
 * @file B8-H2cServer.cc
 * @brief HTTP/2 Cleartext (h2c) 服务器压力测试
 * @details 高性能 H2c 服务器，用于接收客户端压测
 */

#include "galay-http/kernel/http2/Http2Server.h"
#include "galay-http/kernel/http/HttpLog.h"
#include <iostream>
#include <atomic>
#include <chrono>

using namespace galay::http2;
using namespace galay::kernel;

// 统计数据
std::atomic<uint64_t> total_requests{0};
std::atomic<uint64_t> total_bytes{0};
std::chrono::steady_clock::time_point start_time;

/**
 * @brief 处理 HTTP/2 请求
 */
Coroutine handleRequest(Http2ConnImpl<TcpSocket>& conn, Http2Stream::ptr stream, Http2Request request) {
    total_requests++;

    // 构造响应
    Http2Response response;
    response.status = 200;
    response.headers.push_back({"content-type", "text/plain"});

    std::string body = "Hello from H2c Server!";
    response.headers.push_back({"content-length", std::to_string(body.size())});
    response.body = body;

    total_bytes += body.size();

    // 发送响应
    std::vector<Http2HeaderField> headers;
    headers.push_back({":status", std::to_string(response.status)});
    for (const auto& h : response.headers) {
        headers.push_back(h);
    }

    // 发送 HEADERS 帧
    while (true) {
        auto result = co_await conn.sendHeaders(stream->streamId(), headers, false, true);
        if (!result) {
            HTTP_LOG_ERROR("[h2c] [headers] [send-fail]");
            co_return;
        }
        if (result.value()) break;
    }

    // 发送 DATA 帧
    while (true) {
        auto result = co_await conn.sendDataFrame(stream->streamId(), response.body, true);
        if (!result) {
            HTTP_LOG_ERROR("[h2c] [data] [send-fail]");
            co_return;
        }
        if (result.value()) break;
    }

    co_return;
}

/**
 * @brief 统计协程
 */
Coroutine statsCoroutine() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(5));

        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - start_time);

        if (duration.count() > 0) {
            uint64_t requests = total_requests.load();
            uint64_t bytes = total_bytes.load();

            double rps = requests / static_cast<double>(duration.count());
            double mbps = (bytes / 1024.0 / 1024.0) / static_cast<double>(duration.count());

            std::cout << "\r[Stats] Requests: " << requests
                      << " | RPS: " << static_cast<int>(rps)
                      << " | Throughput: " << mbps << " MB/s" << std::flush;
        }
    }
    co_return;
}

int main(int argc, char* argv[]) {
    uint16_t port = 9080;
    if (argc > 1) {
        port = std::atoi(argv[1]);
    }

    std::cout << "========================================\n";
    std::cout << "HTTP/2 Cleartext (h2c) 服务器压测\n";
    std::cout << "========================================\n";
    std::cout << "监听端口: " << port << "\n";
    std::cout << "测试命令: ./build/benchmark/B8-H2cClient localhost " << port << " <并发数> <请求数>\n";
    std::cout << "========================================\n\n";

    try {
        H2cServerConfig config;
        config.host = "0.0.0.0";
        config.port = port;
        config.io_scheduler_count = 4;
        config.compute_scheduler_count = 0;
        config.max_concurrent_streams = 1000;
        config.initial_window_size = 65535;

        H2cServer server(config);

        start_time = std::chrono::steady_clock::now();

        // 启动统计协程
        server.getRuntime().getNextIOScheduler()->spawn(statsCoroutine());

        server.start(handleRequest);

        std::cout << "服务器已启动，按 Ctrl+C 停止...\n\n";

        // 保持运行
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
