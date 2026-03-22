/**
 * @file E8-WssClient.cc
 * @brief WSS (WebSocket Secure) 客户端示例
 * @details 演示如何使用 WssClient 连接到 WSS 服务器
 */

#include <iostream>
#include <chrono>
#include "galay-http/kernel/http/HttpLog.h"
#include "galay-http/kernel/websocket/WsClient.h"
#include "galay-http/protoc/websocket/WebSocketFrame.h"
#include "galay-kernel/kernel/Runtime.h"

#ifdef GALAY_HTTP_SSL_ENABLED

using namespace galay::http;
using namespace galay::websocket;
using namespace galay::kernel;

static std::atomic<bool> g_done{false};

/**
 * @brief WSS 客户端协程
 */
Coroutine wssClientCoroutine(const std::string& url, int message_count) {
    try {
        constexpr auto kOpTimeout = std::chrono::milliseconds(3000);

        // 1. 创建 WssClient
        WssClient client(WssClientBuilder()
            .verifyPeer(false)  // 跳过证书验证（用于自签名证书）
            .build());

        // 2. TCP 连接
        auto connect_result = co_await client.connect(url).timeout(kOpTimeout);
        if (!connect_result) {
            HTTP_LOG_ERROR("[connect] [fail] [{}]", connect_result.error().message());
            g_done = true;
            co_return;
        }
        HTTP_LOG_INFO("[connect] [ok]");

        // 3. SSL 握手
        auto handshake_result = co_await client.handshake();
        if (!handshake_result) {
            HTTP_LOG_ERROR("[ssl] [handshake-fail] [{}]", handshake_result.error().message());
            g_done = true;
            co_await client.close();
            co_return;
        }
        HTTP_LOG_INFO("[ssl] [handshake-ok]");

        // 4. 获取 Session 并升级 WebSocket
        auto session = client.getSession(WsWriterSetting::byClient());
        auto upgrader = session.upgrade();
        auto upgrade_result = co_await upgrader().timeout(kOpTimeout);
        if (!upgrade_result) {
            HTTP_LOG_ERROR("[ws] [upgrade] [fail] [{}]", upgrade_result.error().message());
            g_done = true;
            co_await client.close();
            co_return;
        }
        if (!upgrade_result.value()) {
            HTTP_LOG_ERROR("[ws] [upgrade] [incomplete]");
            g_done = true;
            co_await client.close();
            co_return;
        }
        HTTP_LOG_INFO("[ws] [upgrade] [ok]");

        // 5. 接收欢迎消息
        std::string welcome_msg;
        WsOpcode welcome_opcode;
        while (true) {
            auto recv_result = co_await session.getMessage(welcome_msg, welcome_opcode).timeout(kOpTimeout);
            if (!recv_result) {
                HTTP_LOG_ERROR("[ws] [welcome] [recv-fail] [{}]", recv_result.error().message());
                g_done = true;
                co_await client.close();
                co_return;
            }
            if (recv_result.value()) {
                break;
            }
        }
        HTTP_LOG_INFO("[ws] [recv] [msg={}]", welcome_msg);

        // 6. 发送和接收消息
        for (int i = 0; i < message_count; i++) {
            std::string msg = "Hello WSS #" + std::to_string(i + 1);

            // 发送文本消息
            while (true) {
                auto send_result = co_await session.sendText(msg);
                if (!send_result) {
                    HTTP_LOG_ERROR("[ws] [send-fail] [{}]", send_result.error().message());
                    g_done = true;
                    co_await client.close();
                    co_return;
                }
                if (send_result.value()) {
                    break;
                }
            }
            HTTP_LOG_INFO("[ws] [send] [msg={}]", msg);

            // 接收回显
            std::string echo_msg;
            WsOpcode echo_opcode;
            while (true) {
                auto recv_result = co_await session.getMessage(echo_msg, echo_opcode).timeout(kOpTimeout);
                if (!recv_result) {
                    HTTP_LOG_ERROR("[ws] [recv-fail] [{}]", recv_result.error().message());
                    g_done = true;
                    co_await client.close();
                    co_return;
                }
                if (recv_result.value()) {
                    break;
                }
            }
            HTTP_LOG_INFO("[ws] [recv] [msg={}]", echo_msg);
        }

        // 7. 发送关闭帧
        HTTP_LOG_INFO("[ws] [close] [send]");
        while (true) {
            auto close_result = co_await session.sendClose(WsCloseCode::Normal);
            if (!close_result) {
                HTTP_LOG_ERROR("[ws] [close-fail] [{}]", close_result.error().message());
                break;
            }
            if (close_result.value()) {
                break;
            }
        }

        co_await client.close();
        HTTP_LOG_INFO("[ws] [conn] [closed]");

    } catch (const std::exception& e) {
        HTTP_LOG_ERROR("[exception] [{}]", e.what());
    }

    g_done = true;
    co_return;
}

int main(int argc, char* argv[]) {
    std::string url = "wss://localhost:8443/ws";
    int message_count = 5;

    if (argc > 1) url = argv[1];
    if (argc > 2) message_count = std::atoi(argv[2]);

    std::cout << "========================================\n";
    std::cout << "WSS (WebSocket Secure) Client Example\n";
    std::cout << "========================================\n";
    std::cout << "URL: " << url << "\n";
    std::cout << "Messages: " << message_count << "\n";
    std::cout << "========================================\n";

    try {
        Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
        runtime.start();

        auto* scheduler = runtime.getNextIOScheduler();
        if (!scheduler) {
            std::cerr << "No IO scheduler available\n";
            return 1;
        }

        scheduleCoroutine(scheduler, wssClientCoroutine(url, message_count));

        // 等待完成
        while (!g_done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        runtime.stop();
        std::cout << "Done.\n";

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
