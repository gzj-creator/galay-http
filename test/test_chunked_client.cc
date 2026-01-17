/**
 * @file test_chunked_client.cc
 * @brief HTTP Chunked编码完整测试 - 客户端
 */

#include <iostream>
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

// 发送chunked请求并接收响应
Coroutine sendChunkedRequest() {
    LogInfo("=== HTTP Chunked Client Test ===");
    LogInfo("Connecting to server...");

    TcpSocket client;

    // 设置非阻塞
    auto optResult = client.option().handleNonBlock();
    if (!optResult) {
        LogError("Failed to set non-block: {}", optResult.error().message());
        co_return;
    }

    // 连接到服务器
    Host serverHost(IPType::IPV4, "127.0.0.1", 9999);
    auto connectResult = co_await client.connect(serverHost);
    if (!connectResult) {
        LogError("Failed to connect: {}", connectResult.error().message());
        co_return;
    }

    LogInfo("Connected to server");

    // 创建RingBuffer和HttpReader/Writer
    RingBuffer ringBuffer(8192);
    HttpReaderSetting readerSetting;
    HttpWriterSetting writerSetting;
    HttpReader reader(ringBuffer, readerSetting, client);
    HttpWriter writer(writerSetting, client);

    // 构造chunked请求头
    HttpRequestHeader reqHeader;
    reqHeader.method() = HttpMethod::POST;
    reqHeader.uri() = "/test";
    reqHeader.version() = HttpVersion::HttpVersion_1_1;
    reqHeader.headerPairs().addHeaderPair("Host", "127.0.0.1:9999");
    reqHeader.headerPairs().addHeaderPair("Transfer-Encoding", "chunked");
    reqHeader.headerPairs().addHeaderPair("User-Agent", "galay-http-chunked-client/1.0");

    // 发送请求头
    LogInfo("Sending request header...");
    auto headerResult = co_await writer.sendHeader(std::move(reqHeader));
    if (!headerResult) {
        LogError("Failed to send header: {}", headerResult.error().message());
        co_await client.close();
        co_return;
    }
    LogInfo("Request header sent: {} bytes", headerResult.value());

    // 发送多个chunk
    LogInfo("Sending chunk 1...");
    std::string chunk1 = "Hello ";
    auto chunk1Result = co_await writer.sendChunk(chunk1, false);
    if (!chunk1Result) {
        LogError("Failed to send chunk1: {}", chunk1Result.error().message());
        co_await client.close();
        co_return;
    }
    LogInfo("Chunk 1 sent: {} bytes", chunk1Result.value());

    LogInfo("Sending chunk 2...");
    std::string chunk2 = "from ";
    auto chunk2Result = co_await writer.sendChunk(chunk2, false);
    if (!chunk2Result) {
        LogError("Failed to send chunk2: {}", chunk2Result.error().message());
        co_await client.close();
        co_return;
    }
    LogInfo("Chunk 2 sent: {} bytes", chunk2Result.value());

    LogInfo("Sending chunk 3...");
    std::string chunk3 = "chunked ";
    auto chunk3Result = co_await writer.sendChunk(chunk3, false);
    if (!chunk3Result) {
        LogError("Failed to send chunk3: {}", chunk3Result.error().message());
        co_await client.close();
        co_return;
    }
    LogInfo("Chunk 3 sent: {} bytes", chunk3Result.value());

    LogInfo("Sending chunk 4...");
    std::string chunk4 = "client!";
    auto chunk4Result = co_await writer.sendChunk(chunk4, false);
    if (!chunk4Result) {
        LogError("Failed to send chunk4: {}", chunk4Result.error().message());
        co_await client.close();
        co_return;
    }
    LogInfo("Chunk 4 sent: {} bytes", chunk4Result.value());

    // 发送最后一个chunk
    LogInfo("Sending last chunk...");
    std::string emptyChunk;
    auto lastChunkResult = co_await writer.sendChunk(emptyChunk, true);
    if (!lastChunkResult) {
        LogError("Failed to send last chunk: {}", lastChunkResult.error().message());
        co_await client.close();
        co_return;
    }
    LogInfo("Last chunk sent: {} bytes", lastChunkResult.value());

    LogInfo("\nAll chunks sent successfully!");
    LogInfo("Waiting for response...\n");

    // 读取响应头
    HttpResponse response;
    bool responseHeaderComplete = false;

    while (!responseHeaderComplete) {
        auto result = co_await reader.getResponse(response);

        if (!result) {
            auto& error = result.error();
            if (error.code() == kConnectionClose) {
                LogInfo("Server closed connection");
            } else {
                LogError("Response parse error: {}", error.message());
            }
            co_await client.close();
            co_return;
        }

        responseHeaderComplete = result.value();
    }

    LogInfo("Response received: {} {}",
            static_cast<int>(response.header().code()),
            response.header().code());

    // 检查响应是否是chunked编码
    if (response.header().isChunked()) {
        LogInfo("Response is chunked encoded");

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
                LogInfo("Received response chunk #{}: {} bytes", chunkCount, chunkData.size());
                allChunkData += chunkData;
            }
        }

        if (isLast) {
            LogInfo("\nAll response chunks received. Total: {} chunks, {} bytes",
                   chunkCount, allChunkData.size());
            LogInfo("Response data:\n{}", allChunkData);
        }
    } else {
        // 非chunked响应
        LogInfo("Response body: {}", response.getBodyStr());
    }

    co_await client.close();
    LogInfo("\nConnection closed");
}

int main() {
    LogInfo("========================================");
    LogInfo("HTTP Chunked Encoding Test - Client");
    LogInfo("========================================\n");

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
    IOSchedulerType scheduler;
    scheduler.start();
    LogInfo("Scheduler started\n");

    // 启动客户端
    scheduler.spawn(sendChunkedRequest());

    // 等待一段时间让测试完成
    std::this_thread::sleep_for(std::chrono::seconds(3));

    scheduler.stop();
    LogInfo("\nTest completed");
#else
    LogWarn("This test requires kqueue (macOS), epoll or io_uring (Linux)");
    return 1;
#endif

    return 0;
}
