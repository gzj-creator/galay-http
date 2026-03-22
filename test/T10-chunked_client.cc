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
#include "galay-http/kernel/http/HttpLog.h"
#include "galay-kernel/kernel/Runtime.h"

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
    HTTP_LOG_INFO("=== HTTP Chunked Client Test ===");
    HTTP_LOG_INFO("Connecting to server...");

    TcpSocket client;

    // 设置非阻塞
    auto optResult = client.option().handleNonBlock();
    if (!optResult) {
        HTTP_LOG_ERROR("Failed to set non-block: {}", optResult.error().message());
        co_return;
    }

    // 连接到服务器
    Host serverHost(IPType::IPV4, "127.0.0.1", 9999);
    auto connectResult = co_await client.connect(serverHost);
    if (!connectResult) {
        HTTP_LOG_ERROR("Failed to connect: {}", connectResult.error().message());
        co_return;
    }

    HTTP_LOG_INFO("Connected to server");

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
    HTTP_LOG_INFO("Sending request header...");
    auto headerResult = co_await writer.sendHeader(std::move(reqHeader));
    if (!headerResult) {
        HTTP_LOG_ERROR("Failed to send header: {}", headerResult.error().message());
        co_await client.close();
        co_return;
    }
    HTTP_LOG_INFO("Request header sent: {} bytes", headerResult.value());

    // 发送多个chunk
    HTTP_LOG_INFO("Sending chunk 1...");
    std::string chunk1 = "Hello ";
    auto chunk1Result = co_await writer.sendChunk(chunk1, false);
    if (!chunk1Result) {
        HTTP_LOG_ERROR("Failed to send chunk1: {}", chunk1Result.error().message());
        co_await client.close();
        co_return;
    }
    HTTP_LOG_INFO("Chunk 1 sent: {} bytes", chunk1Result.value());

    HTTP_LOG_INFO("Sending chunk 2...");
    std::string chunk2 = "from ";
    auto chunk2Result = co_await writer.sendChunk(chunk2, false);
    if (!chunk2Result) {
        HTTP_LOG_ERROR("Failed to send chunk2: {}", chunk2Result.error().message());
        co_await client.close();
        co_return;
    }
    HTTP_LOG_INFO("Chunk 2 sent: {} bytes", chunk2Result.value());

    HTTP_LOG_INFO("Sending chunk 3...");
    std::string chunk3 = "chunked ";
    auto chunk3Result = co_await writer.sendChunk(chunk3, false);
    if (!chunk3Result) {
        HTTP_LOG_ERROR("Failed to send chunk3: {}", chunk3Result.error().message());
        co_await client.close();
        co_return;
    }
    HTTP_LOG_INFO("Chunk 3 sent: {} bytes", chunk3Result.value());

    HTTP_LOG_INFO("Sending chunk 4...");
    std::string chunk4 = "client!";
    auto chunk4Result = co_await writer.sendChunk(chunk4, false);
    if (!chunk4Result) {
        HTTP_LOG_ERROR("Failed to send chunk4: {}", chunk4Result.error().message());
        co_await client.close();
        co_return;
    }
    HTTP_LOG_INFO("Chunk 4 sent: {} bytes", chunk4Result.value());

    // 发送最后一个chunk
    HTTP_LOG_INFO("Sending last chunk...");
    std::string emptyChunk;
    auto lastChunkResult = co_await writer.sendChunk(emptyChunk, true);
    if (!lastChunkResult) {
        HTTP_LOG_ERROR("Failed to send last chunk: {}", lastChunkResult.error().message());
        co_await client.close();
        co_return;
    }
    HTTP_LOG_INFO("Last chunk sent: {} bytes", lastChunkResult.value());

    HTTP_LOG_INFO("\nAll chunks sent successfully!");
    HTTP_LOG_INFO("Waiting for response...\n");

    // 读取响应头
    HttpResponse response;
    bool responseHeaderComplete = false;

    while (!responseHeaderComplete) {
        auto result = co_await reader.getResponse(response);

        if (!result) {
            auto& error = result.error();
            if (error.code() == kConnectionClose) {
                HTTP_LOG_INFO("Server closed connection");
            } else {
                HTTP_LOG_ERROR("Response parse error: {}", error.message());
            }
            co_await client.close();
            co_return;
        }

        responseHeaderComplete = result.value();
    }

    HTTP_LOG_INFO("Response received: {} {}",
            static_cast<int>(response.header().code()),
            httpStatusCodeToString(response.header().code()));

    // 检查响应是否是chunked编码
    if (response.header().isChunked()) {
        HTTP_LOG_INFO("Response is chunked encoded");

        // 读取所有chunk数据
        std::string allChunkData;
        bool isLast = false;
        int chunkCount = 0;

        while (!isLast) {
            std::string chunkData;
            auto chunkResult = co_await reader.getChunk(chunkData);

            if (!chunkResult) {
                auto& error = chunkResult.error();
                HTTP_LOG_ERROR("Chunk parse error: {}", error.message());
                break;
            }

            isLast = chunkResult.value();

            if (!chunkData.empty()) {
                chunkCount++;
                HTTP_LOG_INFO("Received response chunk #{}: {} bytes", chunkCount, chunkData.size());
                allChunkData += chunkData;
            }
        }

        if (isLast) {
            HTTP_LOG_INFO("\nAll response chunks received. Total: {} chunks, {} bytes",
                   chunkCount, allChunkData.size());
            HTTP_LOG_INFO("Response data:\n{}", allChunkData);
        }
    } else {
        // 非chunked响应
        HTTP_LOG_INFO("Response body: {}", response.getBodyStr());
    }

    co_await client.close();
    HTTP_LOG_INFO("\nConnection closed");
}

int main() {
    HTTP_LOG_INFO("========================================");
    HTTP_LOG_INFO("HTTP Chunked Encoding Test - Client");
    HTTP_LOG_INFO("========================================\n");

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
    Runtime rt = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    rt.start();
    HTTP_LOG_INFO("Scheduler started\n");

    auto* scheduler = rt.getNextIOScheduler();
    if (!scheduler) {
        HTTP_LOG_ERROR("Failed to get IO scheduler");
        rt.stop();
        return 1;
    }

    // 启动客户端
    scheduleCoroutine(scheduler, sendChunkedRequest());

    // 等待一段时间让测试完成
    std::this_thread::sleep_for(std::chrono::seconds(3));

    rt.stop();
    HTTP_LOG_INFO("\nTest completed");
#else
    HTTP_LOG_WARN("This test requires kqueue (macOS), epoll or io_uring (Linux)");
    return 1;
#endif

    return 0;
}
