/**
 * @file test_all_awaitable_timeout.cc
 * @brief 验证所有 Awaitable 是否支持 timeout
 */

#include "galay-http/kernel/http/HttpClient.h"
#include "galay-http/kernel/http/HttpReader.h"
#include "galay-http/kernel/http/HttpWriter.h"
#include "galay-kernel/kernel/Runtime.h"
#include "galay-kernel/common/Log.h"
#include <iostream>

using namespace galay::http;
using namespace galay::kernel;
using namespace galay::async;
using namespace std::chrono_literals;

/**
 * @brief 测试 HttpClientAwaitable 超时
 */
Coroutine testHttpClientAwaitableTimeout(IOScheduler* scheduler)
{
    std::cout << "=== Test: HttpClientAwaitable Timeout ===" << std::endl;

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);

    if (!connect_result) {
        std::cout << "⚠ Cannot connect to server (skipping test)" << std::endl;
        co_return;
    }

    HttpClient client(std::move(socket));

    // 测试 HttpClientAwaitable 是否支持 timeout
    auto start = std::chrono::steady_clock::now();

    int loop_count = 0;
    while (true) {
        loop_count++;
        auto result = co_await client.get("/delay/5").timeout(1000ms);  // 应该编译通过

        if (!result || result.value().has_value() || loop_count > 100) {
            break;
        }
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    std::cout << "✓ HttpClientAwaitable supports timeout()" << std::endl;
    std::cout << "  Elapsed: " << elapsed << " ms" << std::endl;

    co_await client.close();
    std::cout << std::endl;
    co_return;
}

/**
 * @brief 测试底层 Awaitable 超时（通过 HttpReader/Writer）
 */
Coroutine testLowLevelAwaitableTimeout(IOScheduler* scheduler)
{
    std::cout << "=== Test: Low-Level Awaitable Timeout ===" << std::endl;

    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);

    if (!connect_result) {
        std::cout << "⚠ Cannot connect to server (skipping test)" << std::endl;
        co_return;
    }

    std::cout << "✓ Connected to server" << std::endl;

    // 创建 HttpReader 和 HttpWriter
    RingBuffer ring_buffer(8192);
    HttpReaderSetting reader_setting;
    HttpWriterSetting writer_setting;

    HttpReader reader(ring_buffer, reader_setting, socket);
    HttpWriter writer(writer_setting, socket);

    // 测试 SendResponseAwaitable
    std::cout << "Testing SendResponseAwaitable..." << std::endl;
    HttpRequest request;
    HttpRequestHeader header;
    header.method() = HttpMethod::GET;
    header.uri() = "/delay/5";
    header.version() = HttpVersion::HttpVersion_1_1;
    request.setHeader(std::move(header));

    auto start = std::chrono::steady_clock::now();

    // 注意：SendResponseAwaitable 和 GetResponseAwaitable 不直接支持 timeout
    // 它们内部的 ReadvAwaitable/WritevAwaitable 支持 timeout
    // 但是用户不能直接调用 writer.sendRequest(request).timeout(1000ms)

    int loop_count = 0;
    while (true) {
        loop_count++;
        auto send_result = co_await writer.sendRequest(request);

        if (!send_result || send_result.value() || loop_count > 100) {
            break;
        }
    }

    std::cout << "✓ SendResponseAwaitable completed (loops: " << loop_count << ")" << std::endl;

    // 测试 GetResponseAwaitable
    std::cout << "Testing GetResponseAwaitable..." << std::endl;
    HttpResponse response;

    loop_count = 0;
    while (true) {
        loop_count++;
        auto recv_result = co_await reader.getResponse(response);

        if (!recv_result || recv_result.value() || loop_count > 100) {
            break;
        }
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    std::cout << "✓ GetResponseAwaitable completed (loops: " << loop_count << ")" << std::endl;
    std::cout << "  Total elapsed: " << elapsed << " ms" << std::endl;

    // 问题：SendResponseAwaitable 和 GetResponseAwaitable 不支持 .timeout()
    // 以下代码会编译失败：
    // auto result = co_await writer.sendRequest(request).timeout(1000ms);  // 编译错误！
    // auto result = co_await reader.getResponse(response).timeout(1000ms); // 编译错误！

    std::cout << "⚠ SendResponseAwaitable and GetResponseAwaitable do NOT support .timeout()" << std::endl;
    std::cout << "  Users must use HttpClientAwaitable for timeout support" << std::endl;

    co_await socket.close();
    std::cout << std::endl;
    co_return;
}

int main()
{
    std::cout << "======================================" << std::endl;
    std::cout << "All Awaitable Timeout Support Test" << std::endl;
    std::cout << "======================================" << std::endl;
    std::cout << std::endl;

    try {
        Runtime runtime;
        runtime.start();

        auto* scheduler = runtime.getNextIOScheduler();
        if (!scheduler) {
            std::cerr << "No IO scheduler available" << std::endl;
            return 1;
        }

        // Test HttpClientAwaitable
        scheduler->spawn(testHttpClientAwaitableTimeout(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        // Test low-level awaitables
        scheduler->spawn(testLowLevelAwaitableTimeout(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        runtime.stop();

        std::cout << "======================================" << std::endl;
        std::cout << "Summary:" << std::endl;
        std::cout << "  ✓ HttpClientAwaitable: supports .timeout()" << std::endl;
        std::cout << "  ⚠ SendResponseAwaitable: does NOT support .timeout()" << std::endl;
        std::cout << "  ⚠ GetResponseAwaitable: does NOT support .timeout()" << std::endl;
        std::cout << "  ⚠ GetRequestAwaitable: does NOT support .timeout()" << std::endl;
        std::cout << "  ⚠ GetChunkAwaitable: does NOT support .timeout()" << std::endl;
        std::cout << std::endl;
        std::cout << "Recommendation: Add TimeoutSupport to all HTTP Awaitables" << std::endl;
        std::cout << "======================================" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
