/**
 * @file bench_websocket_client.cc
 * @brief WebSocket 客户端压测程序
 */

#include <iostream>
#include <string>
#include <atomic>
#include <chrono>
#include <vector>
#include <algorithm>
#include <numeric>
#include <thread>
#include "galay-kernel/concurrency/AsyncMutex.h"
#include "galay-http/kernel/http/HttpClient.h"
#include "galay-http/kernel/websocket/WsConn.h"
#include "galay-http/utils/Http1_1RequestBuilder.h"
#include "galay-kernel/kernel/Runtime.h"
#include "galay-http/kernel/http/HttpLog.h"
#include "kernel/websocket/WsClient.h"
#include "kernel/websocket/WsWriterSetting.h"

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
std::atomic<bool> g_stop{false};
std::atomic<int> g_active_clients{0};
AsyncMutex g_latency_mutex;
std::vector<uint32_t> g_latencies_us;

struct ClientGuard {
    explicit ClientGuard(std::atomic<int>& counter)
        : m_counter(&counter) {
        m_counter->fetch_add(1);
    }
    ~ClientGuard() {
        m_counter->fetch_sub(1);
    }
private:
    std::atomic<int>* m_counter;
};

static uint32_t toLatencyUs(std::chrono::steady_clock::duration duration) {
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    if (us < 0) {
        return 0;
    }
    return static_cast<uint32_t>(us);
}

/**
 * @brief 单个 WebSocket 客户端压测
 */
Coroutine benchmarkWebSocketClient(
    IOScheduler* scheduler,
    int client_id,
    const std::string& message_payload,
    std::chrono::steady_clock::time_point end_time)
{
    (void)scheduler;
    ClientGuard guard(g_active_clients);
    g_total_connections.fetch_add(1);

    WsClient client;

    auto connect_result = co_await client.connect("ws://127.0.0.1:8080/ws");
    if (!connect_result) {
        HTTP_LOG_ERROR("[client] [{}] [connect-fail] [{}]", client_id, connect_result.error().message());
        g_failed_connections.fetch_add(1);
        co_return;
    }

    auto session = client.getSession(WsWriterSetting::byClient());
    auto upgrader = session.upgrade();
    while(true) {
        auto res = co_await upgrader();
        if(!res) {
            HTTP_LOG_ERROR("[ws] [upgrade] [fail] [{}]", res.error().message());
            co_return;
        }
        if(res.value()) {
            break;
        } 
    }

    HTTP_LOG_INFO("[ws] [upgrade] [ok]");

    g_successful_connections.fetch_add(1);

    auto ws_reader = session.getReader();
    auto ws_writer = session.getWriter();
    // 读取欢迎消息
    std::string welcome_msg;
    WsOpcode welcome_opcode;
    while(true) {
        auto welcome_result = co_await ws_reader.getMessage(welcome_msg, welcome_opcode);
        if(!welcome_result) {
            HTTP_LOG_ERROR("[ws] [welcome] [recv-fail] [{}]", welcome_result.error().message());
            co_return;
        }
        if(welcome_result.value()) {
            g_total_messages_received.fetch_add(1);
            g_total_bytes_received.fetch_add(welcome_msg.size());
            break;
        }
    }

    // 发送和接收消息（固定时间压测）
    while (!g_stop.load(std::memory_order_relaxed) &&
           std::chrono::steady_clock::now() < end_time) {
        auto round_start = std::chrono::steady_clock::now();
        // 发送消息
        WsFrame frame;
        frame.header.fin = true;
        frame.header.opcode = WsOpcode::Text;
        frame.header.mask = true;  // 客户端必须设置 mask
        frame.payload = message_payload;
        frame.header.payload_length = message_payload.size();

        while(true) {
            auto send_result = co_await ws_writer.sendFrame(frame);
            if (!send_result) {
                HTTP_LOG_ERROR("[client] [{}] [send-fail] [{}]", client_id, send_result.error().message());
                co_return;
            }
            if(send_result.value()) {
                g_total_messages_sent.fetch_add(1);
                g_total_bytes_sent.fetch_add(message_payload.size());
                break;
            }
        }
        // 读取回显消息
        std::string echo_msg;
        WsOpcode echo_opcode;
        while(true) {
            auto echo_result = co_await ws_reader.getMessage(echo_msg, echo_opcode);
            if (echo_result.has_value() && echo_result.value()) {
                g_total_messages_received.fetch_add(1);
                g_total_bytes_received.fetch_add(echo_msg.size());
                auto lock_result = co_await g_latency_mutex.lock();
                if (lock_result.has_value()) {
                    g_latencies_us.push_back(toLatencyUs(std::chrono::steady_clock::now() - round_start));
                    g_latency_mutex.unlock();
                }
                break;
            } else {
                HTTP_LOG_ERROR("[client] [{}] [echo] [recv-fail]", client_id);
                co_return;
            }
        }
        
    }

    // 关闭连接
    co_await client.close();
    co_return;
}

/**
 * @brief 打印统计信息
 */
void printStats(const std::chrono::steady_clock::time_point& start_time,
                const std::chrono::steady_clock::time_point& end_time) {
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    double duration_sec = duration / 1000.0;

    std::vector<uint32_t> latencies;
    while (!g_latency_mutex.tryLock()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    latencies = g_latencies_us;
    g_latency_mutex.unlock();

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
    if (!latencies.empty()) {
        std::sort(latencies.begin(), latencies.end());
        uint64_t sum_us = std::accumulate(latencies.begin(), latencies.end(), uint64_t(0));
        auto pick = [&](double p) -> uint32_t {
            size_t idx = static_cast<size_t>(p * (latencies.size() - 1));
            return latencies[idx];
        };
        double avg_ms = static_cast<double>(sum_us) / latencies.size() / 1000.0;
        std::cout << "\nLatency (RTT):\n";
        std::cout << "  Count:    " << latencies.size() << "\n";
        std::cout << "  Min:      " << (latencies.front() / 1000.0) << " ms\n";
        std::cout << "  Avg:      " << avg_ms << " ms\n";
        std::cout << "  P50:      " << (pick(0.50) / 1000.0) << " ms\n";
        std::cout << "  P95:      " << (pick(0.95) / 1000.0) << " ms\n";
        std::cout << "  P99:      " << (pick(0.99) / 1000.0) << " ms\n";
        std::cout << "  Max:      " << (latencies.back() / 1000.0) << " ms\n";
    }
    std::cout << "========================================\n";
}

int main(int argc, char* argv[]) {
    // 解析命令行参数
    int num_clients = 10;
    double duration_sec = 10.0;
    int message_size = 1024;

    if (argc > 1) num_clients = std::stoi(argv[1]);
    if (argc > 2) duration_sec = std::stod(argv[2]);
    if (argc > 3) message_size = std::stoi(argv[3]);

    std::cout << "========================================\n";
    std::cout << "WebSocket Client Benchmark\n";
    std::cout << "========================================\n";
    std::cout << "Clients:            " << num_clients << "\n";
    std::cout << "Duration:           " << duration_sec << " seconds\n";
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
    auto duration = std::chrono::duration<double>(duration_sec);
    auto end_time = start_time + std::chrono::duration_cast<std::chrono::steady_clock::duration>(duration);

    // 启动所有客户端
    std::cout << "Starting " << num_clients << " clients...\n";
    for (int i = 0; i < num_clients; i++) {
        scheduler->spawn(benchmarkWebSocketClient(scheduler, i, message_payload, end_time));
    }

    std::cout << "Running for " << duration_sec << " seconds...\n";
    std::this_thread::sleep_for(duration);
    g_stop.store(true);

    auto stop_time = std::chrono::steady_clock::now();
    rt.stop();

    // 打印统计信息
    printStats(start_time, stop_time);

    return 0;
#else
    std::cerr << "No scheduler defined. Please compile with -DUSE_KQUEUE, -DUSE_EPOLL, or -DUSE_IOURING\n";
    return 1;
#endif
}
