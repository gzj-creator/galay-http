/**
 * @file test_reader_writer_server.cc
 * @brief HTTP Reader and Writer 测试 - 服务器端
 */

#include <iostream>
#include <atomic>
#include "galay-http/kernel/http/http_reader.h"
#include "galay-http/kernel/http/http_writer.h"
#include "galay-http/protoc/http/http_request.h"
#include "galay-http/protoc/http/http_response.h"
#include "galay-http/utils/rsp_bld.h"
#include "galay-kernel/async/tcp_socket.h"
#include "galay-kernel/common/buffer.h"
#include "galay-http/kernel/http/http_log.h"
#include "galay-kernel/kernel/runtime.h"

#ifdef USE_KQUEUE
#include "galay-kernel/kernel/kqueue_scheduler.h"
using IOSchedulerType = galay::kernel::KqueueScheduler;
#endif

#ifdef USE_EPOLL
#include "galay-kernel/kernel/epoll_scheduler.h"
using IOSchedulerType = galay::kernel::EpollScheduler;
#endif

#ifdef USE_IOURING
#include "galay-kernel/kernel/io_uring_scheduler.h"
using IOSchedulerType = galay::kernel::IOUringScheduler;
#endif

using namespace galay::http;
using namespace galay::kernel;
using namespace galay::async;

std::atomic<int> g_request_count{0};

// Echo服务器
Task<void> echoServer() {
    HTTP_LOG_INFO("=== HTTP Reader/Writer Test Server ===");
    HTTP_LOG_INFO("Starting server...");

    TcpSocket listener;

    // 设置选项
    auto optResult = listener.option().handleReuseAddr();
    if (!optResult) {
        HTTP_LOG_ERROR("Failed to set reuse addr: {}", optResult.error().message());
        co_return;
    }

    optResult = listener.option().handleNonBlock();
    if (!optResult) {
        HTTP_LOG_ERROR("Failed to set non-block: {}", optResult.error().message());
        co_return;
    }

    // 绑定地址
    Host bindHost(IPType::IPV4, "127.0.0.1", 9999);
    auto bindResult = listener.bind(bindHost);
    if (!bindResult) {
        HTTP_LOG_ERROR("Failed to bind: {}", bindResult.error().message());
        co_return;
    }

    // 监听
    auto listenResult = listener.listen(128);
    if (!listenResult) {
        HTTP_LOG_ERROR("Failed to listen: {}", listenResult.error().message());
        co_return;
    }

    HTTP_LOG_INFO("Server listening on 127.0.0.1:9999");
    HTTP_LOG_INFO("Waiting for client connections...");

    while (true) {
        // 接受连接
        Host clientHost;
        auto acceptResult = co_await listener.accept(&clientHost);
        if (!acceptResult) {
            HTTP_LOG_ERROR("Failed to accept: {}", acceptResult.error().message());
            continue;
        }

        HTTP_LOG_INFO("Client connected from {}:{}", clientHost.ip(), clientHost.port());

        // 创建客户端socket
        TcpSocket client(acceptResult.value());
        client.option().handleNonBlock();

        // 创建RingBuffer和HttpReader
        RingBuffer ringBuffer(8192);
        HttpReaderSetting readerSetting;
        HttpWriterSetting writerSetting;
        HttpReader reader(ringBuffer, readerSetting, client);
        HttpWriter writer(writerSetting, client);

        // 读取HTTP请求
        HttpRequest request;
        bool requestComplete = false;

        while (!requestComplete) {
            // 异步读取数据（getRequest 内部会自动调用 readv）
            auto result = co_await reader.getRequest(request);

            if (!result) {
                auto& error = result.error();
                if (error.code() == kConnectionClose) {
                    HTTP_LOG_INFO("Client disconnected");
                } else {
                    HTTP_LOG_ERROR("Request parse error: {}", error.message());
                }
                break;
            }

            requestComplete = result.value();
        }

        if (requestComplete) {
            g_request_count++;
            HTTP_LOG_INFO("Request #{} received: {} {}",
                    g_request_count.load(),
                    static_cast<int>(request.header().method()),
                    request.header().uri());

            // 测试不同的发送方式
            int testCase = g_request_count.load() % 3;

            if (testCase == 0) {
                // 方式1: 使用 sendResponse 发送完整响应
                std::string body = "Echo: " + request.header().uri() + "\n";
                body += "Request #" + std::to_string(g_request_count.load());
                auto response = Http1_1ResponseBuilder()
                    .status(HttpStatusCode::OK_200)
                    .header("Content-Type", "text/plain")
                    .header("Server", "galay-http-test/1.0")
                    .body(std::move(body))
                    .buildMove();

                auto sendResult = co_await writer.sendResponse(response);
                if (!sendResult) {
                    HTTP_LOG_ERROR("Failed to send response: {}", sendResult.error().message());
                } else {
                    HTTP_LOG_INFO("Response sent (sendResponse): complete");
                }
            } else if (testCase == 1) {
                // 方式2: 使用 sendHeader + send(string) 分离发送
                HttpResponseHeader respHeader;
                respHeader.version() = HttpVersion::HttpVersion_1_1;
                respHeader.code() = HttpStatusCode::OK_200;
                respHeader.headerPairs().addHeaderPair("Content-Type", "text/plain");
                respHeader.headerPairs().addHeaderPair("Server", "galay-http-test/1.0");

                std::string body = "Echo: " + request.header().uri() + "\n";
                body += "Request #" + std::to_string(g_request_count.load());
                respHeader.headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));

                // 发送头部
                auto headerResult = co_await writer.sendHeader(std::move(respHeader));
                if (!headerResult) {
                    HTTP_LOG_ERROR("Failed to send header: {}", headerResult.error().message());
                } else {
                    // 发送body
                    auto bodyResult = co_await writer.send(std::move(body));
                    if (bodyResult) {
                        HTTP_LOG_INFO("Response sent (sendHeader+send): complete");
                    } else {
                        HTTP_LOG_ERROR("Failed to send body: {}", bodyResult.error().message());
                    }
                }
            } else {
                // 方式3: 使用 send(buffer, length) 发送原始数据
                HttpResponseHeader respHeader;
                respHeader.version() = HttpVersion::HttpVersion_1_1;
                respHeader.code() = HttpStatusCode::OK_200;
                respHeader.headerPairs().addHeaderPair("Content-Type", "text/plain");
                respHeader.headerPairs().addHeaderPair("Server", "galay-http-test/1.0");

                std::string body = "Echo: " + request.header().uri() + "\n";
                body += "Request #" + std::to_string(g_request_count.load());
                respHeader.headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));

                std::string headerStr = respHeader.toString();

                // 发送头部（原始数据）
                auto headerResult = co_await writer.send(headerStr.data(), headerStr.size());
                if (!headerResult) {
                    HTTP_LOG_ERROR("Failed to send header: {}", headerResult.error().message());
                } else {
                    // 发送body（原始数据）
                    auto bodyResult = co_await writer.send(body.data(), body.size());
                    if (bodyResult) {
                        HTTP_LOG_INFO("Response sent (send raw): complete");
                    } else {
                        HTTP_LOG_ERROR("Failed to send body: {}", bodyResult.error().message());
                    }
                }
            }
        }

        co_await client.close();
        HTTP_LOG_INFO("Connection closed\n");
    }

    co_await listener.close();
    co_return;
}

int main() {
    HTTP_LOG_INFO("========================================");
    HTTP_LOG_INFO("HTTP Reader/Writer Test - Server");
    HTTP_LOG_INFO("========================================\n");

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
    Runtime rt = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    rt.start();
    HTTP_LOG_INFO("Scheduler started");

    auto* scheduler = rt.getNextIOScheduler();
    if (!scheduler) {
        HTTP_LOG_ERROR("Failed to get IO scheduler");
        rt.stop();
        return 1;
    }

    // 启动服务器
    scheduleTask(scheduler, echoServer());

    HTTP_LOG_INFO("Server is ready. Press Ctrl+C to stop.\n");

    // 保持运行
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    rt.stop();
#else
    HTTP_LOG_WARN("This test requires kqueue (macOS), epoll or io_uring (Linux)");
    return 1;
#endif

    return 0;
}
