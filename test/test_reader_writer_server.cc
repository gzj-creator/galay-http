/**
 * @file test_reader_writer_server.cc
 * @brief HTTP Reader and Writer 测试 - 服务器端
 */

#include <iostream>
#include <atomic>
#include "galay-http/kernel/http/HttpReader.h"
#include "galay-http/kernel/http/HttpWriter.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/common/Buffer.h"
#include "galay-kernel/common/Log.h"

#ifdef USE_KQUEUE
#include "galay-kernel/kernel/KqueueScheduler.h"
using IOSchedulerType = galay::kernel::KqueueScheduler;
#endif

#ifdef USE_EPOLL
#include "galay-kernel/kernel/EpollScheduler.h"
using IOSchedulerType = galay::kernel::EpollScheduler;
#endif

#ifdef USE_IOURING
#include "galay-kernel/kernel/IOUringScheduler.h"
using IOSchedulerType = galay::kernel::IOUringScheduler;
#endif

using namespace galay::http;
using namespace galay::kernel;
using namespace galay::async;

std::atomic<int> g_request_count{0};

// Echo服务器
Coroutine echoServer() {
    LogInfo("=== HTTP Reader/Writer Test Server ===");
    LogInfo("Starting server...");

    TcpSocket listener;

    // 设置选项
    auto optResult = listener.option().handleReuseAddr();
    if (!optResult) {
        LogError("Failed to set reuse addr: {}", optResult.error().message());
        co_return;
    }

    optResult = listener.option().handleNonBlock();
    if (!optResult) {
        LogError("Failed to set non-block: {}", optResult.error().message());
        co_return;
    }

    // 绑定地址
    Host bindHost(IPType::IPV4, "127.0.0.1", 9999);
    auto bindResult = listener.bind(bindHost);
    if (!bindResult) {
        LogError("Failed to bind: {}", bindResult.error().message());
        co_return;
    }

    // 监听
    auto listenResult = listener.listen(128);
    if (!listenResult) {
        LogError("Failed to listen: {}", listenResult.error().message());
        co_return;
    }

    LogInfo("Server listening on 127.0.0.1:9999");
    LogInfo("Waiting for client connections...");

    while (true) {
        // 接受连接
        Host clientHost;
        auto acceptResult = co_await listener.accept(&clientHost);
        if (!acceptResult) {
            LogError("Failed to accept: {}", acceptResult.error().message());
            continue;
        }

        LogInfo("Client connected from {}:{}", clientHost.ip(), clientHost.port());

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
                    LogInfo("Client disconnected");
                } else {
                    LogError("Request parse error: {}", error.message());
                }
                break;
            }

            requestComplete = result.value();
        }

        if (requestComplete) {
            g_request_count++;
            LogInfo("Request #{} received: {} {}",
                    g_request_count.load(),
                    static_cast<int>(request.header().method()),
                    request.header().uri());

            // 测试不同的发送方式
            int testCase = g_request_count.load() % 3;

            if (testCase == 0) {
                // 方式1: 使用 sendResponse 发送完整响应
                HttpResponse response;
                HttpResponseHeader respHeader;
                respHeader.version() = HttpVersion::HttpVersion_1_1;
                respHeader.code() = HttpStatusCode::OK_200;
                respHeader.headerPairs().addHeaderPair("Content-Type", "text/plain");
                respHeader.headerPairs().addHeaderPair("Server", "galay-http-test/1.0");

                std::string body = "Echo: " + request.header().uri() + "\n";
                body += "Request #" + std::to_string(g_request_count.load());
                respHeader.headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));

                response.setHeader(std::move(respHeader));
                response.setBodyStr(std::move(body));

                auto sendResult = co_await writer.sendResponse(response);
                if (sendResult) {
                    LogInfo("Response sent (sendResponse): complete");
                } else {
                    LogError("Failed to send response: {}", sendResult.error().message());
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
                    LogError("Failed to send header: {}", headerResult.error().message());
                } else {
                    // 发送body
                    auto bodyResult = co_await writer.send(std::move(body));
                    if (bodyResult) {
                        LogInfo("Response sent (sendHeader+send): complete");
                    } else {
                        LogError("Failed to send body: {}", bodyResult.error().message());
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
                    LogError("Failed to send header: {}", headerResult.error().message());
                } else {
                    // 发送body（原始数据）
                    auto bodyResult = co_await writer.send(body.data(), body.size());
                    if (bodyResult) {
                        LogInfo("Response sent (send raw): complete");
                    } else {
                        LogError("Failed to send body: {}", bodyResult.error().message());
                    }
                }
            }
        }

        co_await client.close();
        LogInfo("Connection closed\n");
    }

    co_await listener.close();
    co_return;
}

int main() {
    LogInfo("========================================");
    LogInfo("HTTP Reader/Writer Test - Server");
    LogInfo("========================================\n");

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
    IOSchedulerType scheduler;
    scheduler.start();
    LogInfo("Scheduler started");

    // 启动服务器
    scheduler.spawn(echoServer());

    LogInfo("Server is ready. Press Ctrl+C to stop.\n");

    // 保持运行
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    scheduler.stop();
#else
    LogWarn("This test requires kqueue (macOS), epoll or io_uring (Linux)");
    return 1;
#endif

    return 0;
}
