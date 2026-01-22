/**
 * @file bench_websocket_client.cc
 * @brief WebSocket 客户端压测程序
 */

#include <iostream>
#include <string>
#include <atomic>
#include <chrono>
#include <vector>
#include "galay-http/kernel/http/HttpClient.h"
#include "galay-http/kernel/websocket/WsConn.h"
#include "galay-http/kernel/websocket/WsUpgrade.h"
#include "galay-http/utils/Http1_1RequestBuilder.h"
#include "galay-kernel/kernel/Runtime.h"
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
using namespace galay::websocket;
using namespace galay::kernel;

// 统计数据
std::atomic<uint64_t> g_total_connections{0};
std::atomic<uint64_t> g_successful_connections{0};
std::atomic<uint64_t> g_failed_connections{0};
std::atomic<uint64_t> g_total_messages_sent{0};
std::atomic<uint64_t> g_total_messages_received{0};
std::atomic<uint64_t> g_total_bytes_sent{0};
std::atomic<uint64_t> g_total_bytes_received{0};

/**
 * @brief 单个 WebSocket 客户端压测
 */
Coroutine benchmarkWebSocketClient(
    IOScheduler* scheduler,
    int client_id,
    int messages_per_client,
    const std::string& message_payload)
{
    g_total_connections.fetch_add(1);

    // 创建 socket 并连接
    TcpSocket socket(IPType::IPV4);
    auto nonblock_result = socket.option().handleNonBlock();
    if (!nonblock_result) {
        LogError("[Client {}] Failed to set non-block", client_id);
        g_failed_connections.fetch_add(1);
        co_return;
    }

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        LogError("[Client {}] Failed to connect: {}", client_id, connect_result.error().message());
        g_failed_connections.fetch_add(1);
        co_return;
    }

    // 创建 HTTP 客户端
    HttpClient client(std::move(socket));

    // 构建 WebSocket 升级请求
    auto request = Http1_1RequestBuilder::get("/ws")
        .header("Host", "localhost:8080")
        .header("Connection", "Upgrade")
        .header("Upgrade", "websocket")
        .header("Sec-WebSocket-Version", "13")
        .header("Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==")
        .build();

    // 发送升级请求
    auto writer = client.getWriter();
    auto send_result = co_await writer.sendRequest(request);
    if (!send_result) {
        LogError("[Client {}] Failed to send upgrade request: {}", client_id, send_result.error().message());
        g_failed_connections.fetch_add(1);
        co_await client.close();
        co_return;
    }

    // 读取升级响应
    auto reader = client.getReader();
    HttpResponse response;
    bool complete = false;
    while (!complete) {
        auto read_result = co_await reader.getResponse(response);
        if (!read_result) {
            LogError("[Client {}] Failed to read upgrade response: {}", client_id, read_result.error().message());
            g_failed_connections.fetch_add(1);
            co_await client.close();
            co_return;
        }
        complete = read_result.value();
    }

    // 检查升级是否成功
    if (response.header().code() != HttpStatusCode::SwitchingProtocol_101) {
        LogError("[Client {}] WebSocket upgrade failed: {}", client_id, static_cast<int>(response.header().code()));
        g_failed_connections.fetch_add(1);
        co_await client.close();
        co_return;
    }

    g_successful_connections.fetch_add(1);

    // 升级到 WebSocket 连接
    WsReaderSetting reader_setting;
    reader_setting.max_frame_size = 1024 * 1024;
    reader_setting.max_message_size = 10 * 1024 * 1024;

    WsWriterSetting writer_setting;

    WsConn ws_conn(
        std::move(client.socket()),
        std::move(client.ringBuffer()),
        reader_setting,
        writer_setting,
        false  // is_server = false (客户端)
    );

    auto ws_reader = ws_conn.getReader();
    auto ws_writer = ws_conn.getWriter();

    // 读取欢迎消息
    std::string welcome_msg;
    WsOpcode welcome_opcode;
    auto welcome_result = co_await ws_reader.getMessage(welcome_msg, welcome_opcode);
    if (welcome_result.has_value() && welcome_result.value()) {
        g_total_messages_received.fetch_add(1);
        g_total_bytes_received.fetch_add(welcome_msg.size());
    }

    // 发送和接收消息
    for (int i = 0; i < messages_per_client; i++) {
        // 发送消息
        WsFrame frame;
        frame.header.fin = true;
        frame.header.opcode = WsOpcode::Text;
        frame.header.mask = true;  // 客户端必须设置 mask
        frame.payload = message_payload;
        frame.header.payload_length = message_payload.size();

        auto send_result = co_await ws_writer.sendFrame(frame);
        if (!send_result) {
            LogError("[Client {}] Failed to send message {}: {}", client_id, i, send_result.error().message());
            break;
        }

        g_total_messages_sent.fetch_add(1);
        g_total_bytes_sent.fetch_add(message_payload.size());

        // 读取回显消息
        std::string echo_msg;
        WsOpcode echo_opcode;
        auto echo_result = co_await ws_reader.getMessage(echo_msg, echo_opcode);
        if (echo_result.has_value() && echo_result.value()) {
            g_total_messages_received.fetch_add(1);
            g_total_bytes_received.fetch_add(echo_msg.size());
        } else {
            LogError("[Client {}] Failed to read echo message {}", client_id, i);
            break;
        }
    }

    // 关闭连接
    co_await ws_conn.close();
    co_return;
}

/**
 * @brief 打印统计信息
 */
void printStats(const std::chrono::steady_clock::time_point& start_time) {
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    double duration_sec = duration / 1000.0;

    std::cout << "\n========================================\n";
    std::cout << "WebSocket Benchmark Results\n";
    std::cout << "========================================\n";
    std::cout << "Duration: " << duration_sec << " seconds\n";
    std::cout << "\nConnections:\n";
    std::cout << "  Total:      " << g_total_connections.load() << "\n";
    std::cout << "  Successful: " << g_successful_connections.load() << "\n";
    std::cout << "  Failed:     " << g_failed_connections.load() << "\n";
    std::cout << "\nMessages:\n";
    std::cout << "  Sent:       " << g_total_messages_sent.load() << "\n";
    std::cout << "  Received:   " << g_total_messages_received.load() << "\n";
    std::cout << "\nData Transfer:\n";
    std::cout << "  Sent:       " << g_total_bytes_sent.load() << " bytes ("
              << (g_total_bytes_sent.load() / 1024.0 / 1024.0) << " MB)\n";
    std::cout << "  Received:   " << g_total_bytes_received.load() << " bytes ("
              << (g_total_bytes_received.load() / 1024.0 / 1024.0) << " MB)\n";
    std::cout << "\nThroughput:\n";
    std::cout << "  Messages/sec:  " << (g_total_messages_sent.load() / duration_sec) << "\n";
    std::cout << "  MB/sec (sent): " << (g_total_bytes_sent.load() / 1024.0 / 1024.0 / duration_sec) << "\n";
    std::cout << "  MB/sec (recv): " << (g_total_bytes_received.load() / 1024.0 / 1024.0 / duration_sec) << "\n";
    std::cout << "========================================\n";
}

int main(int argc, char* argv[]) {
    // 解析命令行参数
    int num_clients = 10;
    int messages_per_client = 100;
    int message_size = 1024;

    if (argc > 1) num_clients = std::stoi(argv[1]);
    if (argc > 2) messages_per_client = std::stoi(argv[2]);
    if (argc > 3) message_size = std::stoi(argv[3]);

    std::cout << "========================================\n";
    std::cout << "WebSocket Client Benchmark\n";
    std::cout << "========================================\n";
    std::cout << "Clients:            " << num_clients << "\n";
    std::cout << "Messages per client: " << messages_per_client << "\n";
    std::cout << "Message size:       " << message_size << " bytes\n";
    std::cout << "========================================\n\n";

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
    galay::kernel::Runtime rt;
    rt.start();

    // 获取 IO 调度器
    auto* scheduler = rt.getNextIOScheduler();
    if (!scheduler) {
        std::cerr << "Failed to get IO scheduler\n";
        return 1;
    }

    // 生成测试消息
    std::string message_payload(message_size, 'A');

    // 记录开始时间
    auto start_time = std::chrono::steady_clock::now();

    // 启动所有客户端
    std::cout << "Starting " << num_clients << " clients...\n";
    for (int i = 0; i < num_clients; i++) {
        scheduler->spawn(benchmarkWebSocketClient(scheduler, i, messages_per_client, message_payload));
    }

    // 等待所有客户端完成（估算时间）
    int estimated_time = (num_clients * messages_per_client) / 100 + 10;  // 粗略估算
    std::cout << "Waiting for clients to complete (estimated " << estimated_time << " seconds)...\n";
    std::this_thread::sleep_for(std::chrono::seconds(estimated_time));

    rt.stop();

    // 打印统计信息
    printStats(start_time);

    return 0;
#else
    std::cerr << "No scheduler defined. Please compile with -DUSE_KQUEUE, -DUSE_EPOLL, or -DUSE_IOURING\n";
    return 1;
#endif
}
