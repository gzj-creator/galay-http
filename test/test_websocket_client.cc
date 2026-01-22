/**
 * @file test_websocket_client.cc
 * @brief WebSocket 客户端测试程序
 */

#include <iostream>
#include <string>
#include "galay-http/kernel/http/HttpClient.h"
#include "galay-http/kernel/websocket/WsConn.h"
#include "galay-http/kernel/websocket/WsUpgrade.h"
#include "galay-http/utils/Http1_1RequestBuilder.h"
#include "galay-kernel/kernel/Runtime.h"
#include "galay-kernel/common/Log.h"
#include "galay-kernel/common/Sleep.hpp"

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

/**
 * @brief WebSocket 客户端测试
 */
Coroutine testWebSocketClient(IOScheduler* scheduler) {
    LogInfo("Starting WebSocket client test");

    // 创建 socket 并连接
    TcpSocket socket(IPType::IPV4);
    auto nonblock_result = socket.option().handleNonBlock();
    if (!nonblock_result) {
        LogError("Failed to set non-block");
        co_return;
    }

    Host host(IPType::IPV4, "127.0.0.1", 8080);
    auto connect_result = co_await socket.connect(host);
    if (!connect_result) {
        LogError("Failed to connect to server: {}", connect_result.error().message());
        co_return;
    }

    LogInfo("Connected to server");

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

    LogInfo("Sending WebSocket upgrade request");

    // 发送升级请求
    auto writer = client.getWriter();
    auto send_result = co_await writer.sendRequest(request);
    if (!send_result) {
        LogError("Failed to send upgrade request: {}", send_result.error().message());
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
            LogError("Failed to read upgrade response: {}", read_result.error().message());
            co_await client.close();
            co_return;
        }
        complete = read_result.value();
    }

    // 检查升级是否成功
    if (response.header().code() != HttpStatusCode::SwitchingProtocol_101) {
        LogError("WebSocket upgrade failed: {}", static_cast<int>(response.header().code()));
        co_await client.close();
        co_return;
    }

    LogInfo("WebSocket upgrade successful");

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
        LogInfo("Received welcome message: {}", welcome_msg);
    }

    // 发送测试消息
    for (int i = 1; i <= 5; i++) {
        std::string test_msg = "Test message " + std::to_string(i);
        LogInfo("Sending: {}", test_msg);

        WsFrame frame;
        frame.header.fin = true;
        frame.header.opcode = WsOpcode::Text;
        frame.header.mask = true;  // 客户端必须设置 mask
        frame.payload = test_msg;
        frame.header.payload_length = test_msg.size();

        auto send_result = co_await ws_writer.sendFrame(frame);
        if (!send_result) {
            LogError("Failed to send message: {}", send_result.error().message());
            break;
        }

        // 读取回显消息
        std::string echo_msg;
        WsOpcode echo_opcode;
        auto echo_result = co_await ws_reader.getMessage(echo_msg, echo_opcode);
        if (echo_result.has_value() && echo_result.value()) {
            LogInfo("Received echo: {}", echo_msg);
        } else {
            LogError("Failed to read echo message");
            break;
        }

        // 等待一下
        co_await galay::kernel::sleep(std::chrono::seconds(1));
    }

    // 关闭连接
    LogInfo("Closing WebSocket connection");
    co_await ws_conn.close();

    LogInfo("WebSocket client test completed");
    co_return;
}

int main() {
    LogInfo("========================================");
    LogInfo("WebSocket Client Test");
    LogInfo("========================================\n");

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
    galay::kernel::Runtime rt;
    rt.start();

    // 获取 IO 调度器并启动测试协程
    auto* scheduler = rt.getNextIOScheduler();
    if (!scheduler) {
        LogError("Failed to get IO scheduler");
        return 1;
    }

    scheduler->spawn(testWebSocketClient(scheduler));

    // 等待测试完成（这里简单地等待一段时间）
    std::this_thread::sleep_for(std::chrono::seconds(30));

    rt.stop();
    LogInfo("Test completed");
    return 0;
#else
    LogError("No scheduler defined. Please compile with -DUSE_KQUEUE, -DUSE_EPOLL, or -DUSE_IOURING");
    return 1;
#endif
}
