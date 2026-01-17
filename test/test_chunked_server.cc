/**
 * @file test_chunked_server.cc
 * @brief HTTP Chunked编码完整测试 - 服务器端
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

// 处理客户端连接
Coroutine handleClient(TcpSocket client, Host clientHost) {
    LogInfo("Client connected from {}:{}", clientHost.ip(), clientHost.port());

    client.option().handleNonBlock();

    // 创建RingBuffer和HttpReader/Writer
    RingBuffer ringBuffer(8192);
    HttpReaderSetting readerSetting;
    HttpWriterSetting writerSetting;
    HttpReader reader(ringBuffer, readerSetting, client);
    HttpWriter writer(writerSetting, client);

    // 读取HTTP请求头
    HttpRequest request;
    bool requestHeaderComplete = false;

    while (!requestHeaderComplete) {
        auto result = co_await reader.getRequest(request);

        if (!result) {
            auto& error = result.error();
            if (error.code() == kConnectionClose) {
                LogInfo("Client disconnected");
            } else {
                LogError("Request parse error: {}", error.message());
            }
            co_await client.close();
            co_return;
        }

        requestHeaderComplete = result.value();
    }

    g_request_count++;
    LogInfo("Request #{} received: {} {}",
            g_request_count.load(),
            static_cast<int>(request.header().method()),
            request.header().uri());

    // 检查是否是chunked编码
    if (request.header().isChunked()) {
        LogInfo("Detected chunked transfer encoding");

        // 读取所有chunk数据
        std::string allChunkData;
        bool isLast = false;
        int chunkCount = 0;

        while (!isLast) {
            std::string chunkData;
            auto chunkResult = co_await reader.getChunk(chunkData);

            if (!chunkResult) {
                auto& error = chunkResult.error();
                LogError("Chunk parse error: {}", error.message());
                break;
            }

            isLast = chunkResult.value();

            if (!chunkData.empty()) {
                chunkCount++;
                LogInfo("Received chunk #{}: {} bytes", chunkCount, chunkData.size());
                allChunkData += chunkData;
            }
        }

        if (isLast) {
            LogInfo("All chunks received. Total: {} chunks, {} bytes",
                   chunkCount, allChunkData.size());
            LogInfo("Chunk data: {}", allChunkData);

            // 发送chunked响应
            HttpResponseHeader respHeader;
            respHeader.version() = HttpVersion::HttpVersion_1_1;
            respHeader.code() = HttpStatusCode::OK_200;
            respHeader.headerPairs().addHeaderPair("Content-Type", "text/plain");
            respHeader.headerPairs().addHeaderPair("Transfer-Encoding", "chunked");
            respHeader.headerPairs().addHeaderPair("Server", "galay-http-chunked-test/1.0");

            // 发送响应头
            auto headerResult = co_await writer.sendHeader(std::move(respHeader));
            if (!headerResult) {
                LogError("Failed to send header: {}", headerResult.error().message());
                co_await client.close();
                co_return;
            }

            // 发送多个chunk
            std::string chunk1 = "Received " + std::to_string(chunkCount) + " chunks\n";
            auto chunk1Result = co_await writer.sendChunk(chunk1, false);
            if (!chunk1Result) {
                LogError("Failed to send chunk1: {}", chunk1Result.error().message());
                co_await client.close();
                co_return;
            }

            std::string chunk2 = "Total bytes: " + std::to_string(allChunkData.size()) + "\n";
            auto chunk2Result = co_await writer.sendChunk(chunk2, false);
            if (!chunk2Result) {
                LogError("Failed to send chunk2: {}", chunk2Result.error().message());
                co_await client.close();
                co_return;
            }

            std::string chunk3 = "Echo: " + allChunkData;
            auto chunk3Result = co_await writer.sendChunk(chunk3, false);
            if (!chunk3Result) {
                LogError("Failed to send chunk3: {}", chunk3Result.error().message());
                co_await client.close();
                co_return;
            }

            // 发送最后一个chunk
            std::string emptyChunk;
            auto lastChunkResult = co_await writer.sendChunk(emptyChunk, true);
            if (!lastChunkResult) {
                LogError("Failed to send last chunk: {}", lastChunkResult.error().message());
                co_await client.close();
                co_return;
            }

            LogInfo("Chunked response sent successfully");
        }
    } else {
        // 非chunked请求
        LogInfo("Non-chunked request");

        // 发送简单响应
        HttpResponse response;
        HttpResponseHeader respHeader;
        respHeader.version() = HttpVersion::HttpVersion_1_1;
        respHeader.code() = HttpStatusCode::OK_200;
        respHeader.headerPairs().addHeaderPair("Content-Type", "text/plain");
        respHeader.headerPairs().addHeaderPair("Server", "galay-http-chunked-test/1.0");

        std::string body = "Non-chunked request received\n";
        respHeader.headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));

        response.setHeader(std::move(respHeader));
        response.setBodyStr(std::move(body));

        auto sendResult = co_await writer.sendResponse(response);
        if (sendResult) {
            LogInfo("Response sent: {} bytes", sendResult.value());
        } else {
            LogError("Failed to send response: {}", sendResult.error().message());
        }
    }

    co_await client.close();
    LogInfo("Connection closed\n");
}

// Chunk测试服务器
Coroutine chunkedTestServer() {
    LogInfo("=== HTTP Chunked Encoding Test Server ===");
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

        // 创建RingBuffer和HttpReader/Writer
        RingBuffer ringBuffer(8192);
        HttpReaderSetting readerSetting;
        HttpWriterSetting writerSetting;
        HttpReader reader(ringBuffer, readerSetting, client);
        HttpWriter writer(writerSetting, client);

        // 读取HTTP请求头
        HttpRequest request;
        bool requestHeaderComplete = false;

        while (!requestHeaderComplete) {
            auto result = co_await reader.getRequest(request);

            if (!result) {
                auto& error = result.error();
                if (error.code() == kConnectionClose) {
                    LogInfo("Client disconnected");
                } else {
                    LogError("Request parse error: {}", error.message());
                }
                co_await client.close();
                continue;
            }

            requestHeaderComplete = result.value();
        }

        if (!requestHeaderComplete) {
            co_await client.close();
            continue;
        }

        g_request_count++;
        LogInfo("Request #{} received: {} {}",
                g_request_count.load(),
                static_cast<int>(request.header().method()),
                request.header().uri());

        // 检查是否是chunked编码
        if (request.header().isChunked()) {
            LogInfo("Detected chunked transfer encoding");

            // 读取所有chunk数据
            std::string allChunkData;
            bool isLast = false;
            int chunkCount = 0;

            while (!isLast) {
                std::string chunkData;
                auto chunkResult = co_await reader.getChunk(chunkData);

                if (!chunkResult) {
                    auto& error = chunkResult.error();
                    LogError("Chunk parse error: {}", error.message());
                    break;
                }

                isLast = chunkResult.value();

                if (!chunkData.empty()) {
                    chunkCount++;
                    LogInfo("Received chunk #{}: {} bytes", chunkCount, chunkData.size());
                    allChunkData += chunkData;
                }
            }

            if (isLast) {
                LogInfo("All chunks received. Total: {} chunks, {} bytes",
                       chunkCount, allChunkData.size());
                LogInfo("Chunk data: {}", allChunkData);

                // 发送chunked响应
                HttpResponseHeader respHeader;
                respHeader.version() = HttpVersion::HttpVersion_1_1;
                respHeader.code() = HttpStatusCode::OK_200;
                respHeader.headerPairs().addHeaderPair("Content-Type", "text/plain");
                respHeader.headerPairs().addHeaderPair("Transfer-Encoding", "chunked");
                respHeader.headerPairs().addHeaderPair("Server", "galay-http-chunked-test/1.0");

                // 发送响应头
                auto headerResult = co_await writer.sendHeader(std::move(respHeader));
                if (!headerResult) {
                    LogError("Failed to send header: {}", headerResult.error().message());
                    co_await client.close();
                    continue;
                }

                // 发送多个chunk
                std::string chunk1 = "Received " + std::to_string(chunkCount) + " chunks\n";
                auto chunk1Result = co_await writer.sendChunk(chunk1, false);
                if (!chunk1Result) {
                    LogError("Failed to send chunk1: {}", chunk1Result.error().message());
                    co_await client.close();
                    continue;
                }

                std::string chunk2 = "Total bytes: " + std::to_string(allChunkData.size()) + "\n";
                auto chunk2Result = co_await writer.sendChunk(chunk2, false);
                if (!chunk2Result) {
                    LogError("Failed to send chunk2: {}", chunk2Result.error().message());
                    co_await client.close();
                    continue;
                }

                std::string chunk3 = "Echo: " + allChunkData;
                auto chunk3Result = co_await writer.sendChunk(chunk3, false);
                if (!chunk3Result) {
                    LogError("Failed to send chunk3: {}", chunk3Result.error().message());
                    co_await client.close();
                    continue;
                }

                // 发送最后一个chunk
                std::string emptyChunk;
                auto lastChunkResult = co_await writer.sendChunk(emptyChunk, true);
                if (!lastChunkResult) {
                    LogError("Failed to send last chunk: {}", lastChunkResult.error().message());
                    co_await client.close();
                    continue;
                }

                LogInfo("Chunked response sent successfully");
            }
        } else {
            // 非chunked请求
            LogInfo("Non-chunked request");

            // 发送简单响应
            HttpResponse response;
            HttpResponseHeader respHeader;
            respHeader.version() = HttpVersion::HttpVersion_1_1;
            respHeader.code() = HttpStatusCode::OK_200;
            respHeader.headerPairs().addHeaderPair("Content-Type", "text/plain");
            respHeader.headerPairs().addHeaderPair("Server", "galay-http-chunked-test/1.0");

            std::string body = "Non-chunked request received\n";
            respHeader.headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));

            response.setHeader(std::move(respHeader));
            response.setBodyStr(std::move(body));

            auto sendResult = co_await writer.sendResponse(response);
            if (sendResult) {
                LogInfo("Response sent: {} bytes", sendResult.value());
            } else {
                LogError("Failed to send response: {}", sendResult.error().message());
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
    LogInfo("HTTP Chunked Encoding Test - Server");
    LogInfo("========================================\n");

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
    IOSchedulerType scheduler;
    scheduler.start();
    LogInfo("Scheduler started");

    // 启动服务器
    scheduler.spawn(chunkedTestServer());

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
