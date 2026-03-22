/**
 * @file B12-WssClient.cc
 * @brief WSS (WebSocket Secure) 客户端压测程序
 * @details 配合 B11-WssServer 进行 WSS 性能测试
 */

#include <iostream>
#include <string>
#include <atomic>
#include <chrono>
#include <thread>
#include <csignal>
#include "galay-http/kernel/http/HttpLog.h"
#include "galay-http/kernel/websocket/WsClient.h"
#include "galay-kernel/kernel/Runtime.h"

#ifdef GALAY_HTTP_SSL_ENABLED

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

// 延迟统计
std::atomic<uint64_t> g_latency_sum_us{0};
std::atomic<uint64_t> g_latency_count{0};
std::atomic<uint32_t> g_latency_min_us{UINT32_MAX};
std::atomic<uint32_t> g_latency_max_us{0};

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
    if (us < 0) return 0;
    return static_cast<uint32_t>(us);
}

/**
 * @brief 单个 WSS 客户端压测协程
 */
Coroutine benchmarkWssClient(
    int client_id,
    const std::string& url,
    const std::string& message_payload,
    std::chrono::steady_clock::time_point end_time)
{
    ClientGuard guard(g_active_clients);
    g_total_connections.fetch_add(1);
    constexpr auto kOpTimeout = std::chrono::milliseconds(3000);

    try {
        // 1. 创建 WssClient
        WssClient client(WssClientBuilder()
            .verifyPeer(false)  // 跳过证书验证（用于自签名证书）
            .build());

        // 2. TCP 连接
        auto connect_result = co_await client.connect(url).timeout(kOpTimeout);
        if (!connect_result) {
            HTTP_LOG_ERROR("[client-{}] [connect-fail] [{}]", client_id, connect_result.error().message());
            g_failed_connections.fetch_add(1);
            co_return;
        }

        // 3. SSL 握手
        auto handshake_result = co_await client.handshake();
        if (!handshake_result) {
            HTTP_LOG_ERROR("[client-{}] [ssl-handshake-fail] [{}]", client_id, handshake_result.error().message());
            g_failed_connections.fetch_add(1);
            co_await client.close();
            co_return;
        }

        // 4. 获取 Session 并升级 WebSocket
        auto session = client.getSession(WsWriterSetting::byClient());
        auto upgrader = session.upgrade();
        auto upgrade_result = co_await upgrader().timeout(kOpTimeout);
        if (!upgrade_result) {
            HTTP_LOG_ERROR("[client-{}] [ws-upgrade-fail] [{}]", client_id, upgrade_result.error().message());
            g_failed_connections.fetch_add(1);
            co_await client.close();
            co_return;
        }
        if (!upgrade_result.value()) {
            HTTP_LOG_ERROR("[client-{}] [ws-upgrade-incomplete]", client_id);
            g_failed_connections.fetch_add(1);
            co_await client.close();
            co_return;
        }

        g_successful_connections.fetch_add(1);

        // 5. 接收欢迎消息
        std::string welcome_msg;
        WsOpcode welcome_opcode;
        while (true) {
            auto recv_result = co_await session.getMessage(welcome_msg, welcome_opcode).timeout(kOpTimeout);
            if (!recv_result) {
                HTTP_LOG_ERROR("[client-{}] [welcome-recv-fail] [{}]", client_id, recv_result.error().message());
                co_await client.close();
                co_return;
            }
            if (recv_result.value()) {
                g_total_messages_received.fetch_add(1);
                g_total_bytes_received.fetch_add(welcome_msg.size());
                break;
            }
        }

        // 6. 发送和接收消息（固定时间压测）
        while (!g_stop.load(std::memory_order_relaxed) &&
               std::chrono::steady_clock::now() < end_time) {
            auto round_start = std::chrono::steady_clock::now();

            // 发送消息
            auto send_result = co_await session.sendText(message_payload);
            if (!send_result) {
                HTTP_LOG_ERROR("[client-{}] [send-fail] [{}]", client_id, send_result.error().message());
                goto cleanup;
            }
            g_total_messages_sent.fetch_add(1);
            g_total_bytes_sent.fetch_add(message_payload.size());

            // 接收回显消息
            std::string echo_msg;
            WsOpcode echo_opcode;
            while (true) {
                auto recv_result = co_await session.getMessage(echo_msg, echo_opcode).timeout(kOpTimeout);
                if (!recv_result) {
                    HTTP_LOG_ERROR("[client-{}] [recv-fail] [{}]", client_id, recv_result.error().message());
                    goto cleanup;
                }
                if (recv_result.value()) {
                    g_total_messages_received.fetch_add(1);
                    g_total_bytes_received.fetch_add(echo_msg.size());

                    // 记录延迟统计
                    uint32_t latency_us = toLatencyUs(std::chrono::steady_clock::now() - round_start);
                    g_latency_sum_us.fetch_add(latency_us);
                    g_latency_count.fetch_add(1);

                    // 更新最小值
                    uint32_t old_min = g_latency_min_us.load();
                    while (latency_us < old_min && !g_latency_min_us.compare_exchange_weak(old_min, latency_us));

                    // 更新最大值
                    uint32_t old_max = g_latency_max_us.load();
                    while (latency_us > old_max && !g_latency_max_us.compare_exchange_weak(old_max, latency_us));

                    break;
                }
            }
        }

cleanup:
        // 7. 发送关闭帧
        auto result = co_await session.sendClose(WsCloseCode::Normal);
        if (!result) {
            HTTP_LOG_WARN("[close] [fail] [{}]", result.error().message());
        }

        co_await client.close();

    } catch (const std::exception& e) {
        HTTP_LOG_ERROR("[client-{}] [exception] [{}]", client_id, e.what());
    }

    co_return;
}

/**
 * @brief 打印统计信息
 */
void printStats(const std::chrono::steady_clock::time_point& start_time,
                const std::chrono::steady_clock::time_point& end_time) {
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    double duration_sec = duration / 1000.0;

    std::cout << "\n========================================\n";
    std::cout << "WSS Benchmark Results\n";
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

    uint64_t latency_count = g_latency_count.load();
    if (latency_count > 0) {
        uint64_t latency_sum = g_latency_sum_us.load();
        uint32_t latency_min = g_latency_min_us.load();
        uint32_t latency_max = g_latency_max_us.load();
        double avg_ms = static_cast<double>(latency_sum) / latency_count / 1000.0;

        std::cout << "\nLatency (RTT):\n";
        std::cout << "  Count:    " << latency_count << "\n";
        std::cout << "  Min:      " << (latency_min / 1000.0) << " ms\n";
        std::cout << "  Avg:      " << avg_ms << " ms\n";
        std::cout << "  Max:      " << (latency_max / 1000.0) << " ms\n";
    }
    std::cout << "========================================\n";
}

void signalHandler(int) {
    g_stop = true;
}

int main(int argc, char* argv[]) {
    // 设置日志为文件模式
    galay::http::HttpLogger::file("B12-WssClient.log");

    // 解析命令行参数
    std::string url = "wss://127.0.0.1:8443/ws";
    int num_clients = 10;
    double duration_sec = 10.0;
    int message_size = 1024;

    if (argc > 1) url = argv[1];
    if (argc > 2) num_clients = std::stoi(argv[2]);
    if (argc > 3) duration_sec = std::stod(argv[3]);
    if (argc > 4) message_size = std::stoi(argv[4]);

    std::cout << "========================================\n";
    std::cout << "WSS Client Benchmark\n";
    std::cout << "========================================\n";
    std::cout << "URL:                " << url << "\n";
    std::cout << "Clients:            " << num_clients << "\n";
    std::cout << "Duration:           " << duration_sec << " seconds\n";
    std::cout << "Message size:       " << message_size << " bytes\n";
    std::cout << "========================================\n\n";

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try {
        Runtime runtime = RuntimeBuilder().ioSchedulerCount(4).computeSchedulerCount(0).build();
        runtime.start();

        // 生成测试消息
        std::string message_payload(message_size, 'A');

        // 记录开始时间
        auto start_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration<double>(duration_sec);
        auto end_time = start_time + std::chrono::duration_cast<std::chrono::steady_clock::duration>(duration);

        // 启动所有客户端
        std::cout << "Starting " << num_clients << " clients...\n";
        for (int i = 0; i < num_clients; i++) {
            auto* scheduler = runtime.getNextIOScheduler();
            if (!scheduler) {
                std::cerr << "Failed to get IO scheduler for client " << i << "\n";
                return 1;
            }
            scheduleCoroutine(scheduler, benchmarkWssClient(i, url, message_payload, end_time));
        }

        std::cout << "Running for " << duration_sec << " seconds...\n";
        std::this_thread::sleep_for(duration);
        g_stop.store(true);

        // 等待所有客户端完成
        std::cout << "Waiting for clients to finish...\n";
        const auto wait_deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(static_cast<int>(duration_sec) + 10);
        while (g_active_clients.load() > 0 &&
               std::chrono::steady_clock::now() < wait_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (g_active_clients.load() > 0) {
            std::cerr << "Warning: wait timeout, active_clients=" << g_active_clients.load() << "\n";
        }

        auto stop_time = std::chrono::steady_clock::now();
        runtime.stop();

        // 打印统计信息
        printStats(start_time, stop_time);

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

#else

int main() {
    std::cout << "SSL support is not enabled.\n";
    std::cout << "Rebuild with -DGALAY_HTTP_ENABLE_SSL=ON\n";
    return 0;
}

#endif
