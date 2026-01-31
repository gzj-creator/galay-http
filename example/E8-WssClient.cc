/**
 * @file E8-WssClient.cc
 * @brief WSS (WebSocket Secure) 客户端示例
 * @details 演示如何连接到 WSS 服务器
 */

#include <iostream>
#include <chrono>
#include "galay-http/kernel/http/HttpLog.h"
#include "galay-http/kernel/websocket/WsUpgrade.h"
#include "galay-http/protoc/websocket/WebSocketFrame.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/utils/Http1_1RequestBuilder.h"

#ifdef GALAY_HTTP_SSL_ENABLED

#include "galay-kernel/kernel/Runtime.h"
#include "galay-ssl/SslSocket.h"
#include "galay-ssl/SslContext.h"
#include <galay-utils/algorithm/Base64.hpp>
#include <random>

using namespace galay::http;
using namespace galay::websocket;
using namespace galay::kernel;
using namespace std::chrono_literals;

static std::atomic<bool> g_done{false};

// 生成 WebSocket Key
std::string generateWsKey() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);

    unsigned char random_bytes[16];
    for (int i = 0; i < 16; i++) {
        random_bytes[i] = static_cast<unsigned char>(dis(gen));
    }

    return galay::utils::Base64Util::Base64Encode(random_bytes, 16);
}

/**
 * @brief WSS 客户端协程
 */
Coroutine wssClientCoroutine(const std::string& host, int port, const std::string& path, int message_count) {
    HTTP_LOG_INFO("Connecting to wss://{}:{}{}", host, port, path);

    // 创建 SSL 上下文
    galay::ssl::SslContext ssl_ctx(galay::ssl::SslMethod::TLS_Client);
    ssl_ctx.setVerifyMode(galay::ssl::SslVerifyMode::None);  // 跳过证书验证（用于自签名证书）

    if (!ssl_ctx.isValid()) {
        HTTP_LOG_ERROR("Failed to create SSL context");
        g_done = true;
        co_return;
    }

    // 创建 SSL Socket
    galay::ssl::SslSocket socket(&ssl_ctx, IPType::IPV4);
    socket.option().handleNonBlock();

    // 1. TCP 连接
    Host server_host(IPType::IPV4, host, port);
    auto connect_result = co_await socket.connect(server_host);
    if (!connect_result) {
        HTTP_LOG_ERROR("Connect failed: {}", connect_result.error().message());
        g_done = true;
        co_return;
    }

    HTTP_LOG_INFO("TCP connected, performing SSL handshake...");

    // 2. SSL 握手
    while (!socket.isHandshakeCompleted()) {
        auto handshake_result = co_await socket.handshake();
        if (!handshake_result) {
            auto& err = handshake_result.error();
            if (err.code() == galay::ssl::SslErrorCode::kHandshakeWantRead ||
                err.code() == galay::ssl::SslErrorCode::kHandshakeWantWrite) {
                continue;
            }
            HTTP_LOG_ERROR("SSL handshake failed: {}", err.message());
            g_done = true;
            co_return;
        }
        break;
    }

    HTTP_LOG_INFO("SSL handshake completed, sending WebSocket upgrade request...");

    // 3. 发送 WebSocket 升级请求
    std::string ws_key = generateWsKey();
    HttpRequest upgrade_request = Http1_1RequestBuilder::get(path)
        .host(host + ":" + std::to_string(port))
        .header("Connection", "Upgrade")
        .header("Upgrade", "websocket")
        .header("Sec-WebSocket-Version", "13")
        .header("Sec-WebSocket-Key", ws_key)
        .build();

    std::string request_data = upgrade_request.toString();
    size_t sent = 0;
    while (sent < request_data.size()) {
        auto result = co_await socket.send(request_data.data() + sent, request_data.size() - sent);
        if (!result) {
            HTTP_LOG_ERROR("Failed to send upgrade request: {}", result.error().message());
            g_done = true;
            co_return;
        }
        sent += result.value();
    }

    HTTP_LOG_INFO("Upgrade request sent, waiting for response...");

    // 4. 接收升级响应
    std::vector<char> buffer(4096);
    std::string response_data;
    HttpResponse response;

    while (true) {
        auto recv_result = co_await socket.recv(buffer.data(), buffer.size());
        if (!recv_result) {
            auto& err = recv_result.error();
            // SSL_ERROR_WANT_READ (2) 或 SSL_ERROR_WANT_WRITE (3) 表示需要重试
            if (err.sslError() == SSL_ERROR_WANT_READ || err.sslError() == SSL_ERROR_WANT_WRITE) {
                continue;  // 重试
            }
            HTTP_LOG_ERROR("Failed to receive upgrade response: {}", err.message());
            g_done = true;
            co_return;
        }

        size_t bytes = recv_result.value().size();
        if (bytes == 0) {
            HTTP_LOG_ERROR("Connection closed during upgrade");
            g_done = true;
            co_return;
        }

        response_data.append(buffer.data(), bytes);

        std::vector<iovec> iovecs;
        iovecs.push_back({const_cast<char*>(response_data.data()), response_data.size()});

        auto parse_result = response.fromIOVec(iovecs);
        if (parse_result.first != HttpErrorCode::kNoError) {
            continue;  // 需要更多数据
        }

        if (response.isComplete()) {
            break;
        }
    }

    if (response.header().code() != HttpStatusCode::SwitchingProtocol_101) {
        HTTP_LOG_ERROR("Upgrade failed with status: {}", static_cast<int>(response.header().code()));
        g_done = true;
        co_return;
    }

    // 验证 Sec-WebSocket-Accept
    std::string accept_key = response.header().headerPairs().getValue("Sec-WebSocket-Accept");
    std::string expected_accept = WsUpgrade::generateAcceptKey(ws_key);
    if (accept_key != expected_accept) {
        HTTP_LOG_ERROR("Invalid Sec-WebSocket-Accept");
        g_done = true;
        co_return;
    }

    HTTP_LOG_INFO("WebSocket upgrade successful!");

    // 5. 接收欢迎消息
    std::string accumulated;
    while (true) {
        auto recv_result = co_await socket.recv(buffer.data(), buffer.size());
        if (!recv_result) {
            auto& err = recv_result.error();
            if (err.sslError() == SSL_ERROR_WANT_READ || err.sslError() == SSL_ERROR_WANT_WRITE) {
                continue;
            }
            HTTP_LOG_ERROR("Failed to receive welcome: {}", err.message());
            g_done = true;
            co_return;
        }

        accumulated.append(buffer.data(), recv_result.value().size());

        WsFrame frame;
        std::vector<iovec> iovecs;
        iovecs.push_back({const_cast<char*>(accumulated.data()), accumulated.size()});

        auto parse_result = WsFrameParser::fromIOVec(iovecs, frame, false);
        if (!parse_result) {
            if (parse_result.error().code() == kWsIncomplete) {
                continue;
            }
            HTTP_LOG_ERROR("Frame parse error");
            g_done = true;
            co_return;
        }

        HTTP_LOG_INFO("Received: {}", frame.payload);
        accumulated.erase(0, parse_result.value());
        break;
    }

    // 6. 发送和接收消息
    for (int i = 0; i < message_count; i++) {
        std::string msg = "Hello WSS #" + std::to_string(i + 1);

        // 发送消息（客户端必须使用掩码）
        WsFrame send_frame = WsFrameParser::createTextFrame(msg);
        std::string frame_data = WsFrameParser::toBytes(send_frame, true);  // use_mask = true

        size_t frame_sent = 0;
        while (frame_sent < frame_data.size()) {
            auto result = co_await socket.send(frame_data.data() + frame_sent, frame_data.size() - frame_sent);
            if (!result) {
                HTTP_LOG_ERROR("Send failed");
                g_done = true;
                co_return;
            }
            frame_sent += result.value();
        }
        HTTP_LOG_INFO("Sent: {}", msg);

        // 接收回显
        while (true) {
            auto recv_result = co_await socket.recv(buffer.data(), buffer.size());
            if (!recv_result) {
                auto& err = recv_result.error();
                if (err.sslError() == SSL_ERROR_WANT_READ || err.sslError() == SSL_ERROR_WANT_WRITE) {
                    continue;
                }
                HTTP_LOG_ERROR("Receive failed: {}", err.message());
                g_done = true;
                co_return;
            }

            accumulated.append(buffer.data(), recv_result.value().size());

            WsFrame recv_frame;
            std::vector<iovec> iovecs;
            iovecs.push_back({const_cast<char*>(accumulated.data()), accumulated.size()});

            auto parse_result = WsFrameParser::fromIOVec(iovecs, recv_frame, false);
            if (!parse_result) {
                if (parse_result.error().code() == kWsIncomplete) {
                    continue;
                }
                HTTP_LOG_ERROR("Frame parse error");
                g_done = true;
                co_return;
            }

            HTTP_LOG_INFO("Received: {}", recv_frame.payload);
            accumulated.erase(0, parse_result.value());
            break;
        }
    }

    // 7. 发送关闭帧
    HTTP_LOG_INFO("Sending close frame...");
    WsFrame close_frame = WsFrameParser::createCloseFrame(WsCloseCode::Normal);
    std::string close_data = WsFrameParser::toBytes(close_frame, true);
    co_await socket.send(close_data.data(), close_data.size());

    co_await socket.close();
    HTTP_LOG_INFO("Connection closed");
    g_done = true;
    co_return;
}

int main(int argc, char* argv[]) {
    std::string host = "localhost";
    int port = 8443;
    std::string path = "/ws";
    int message_count = 5;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = std::atoi(argv[2]);
    if (argc > 3) path = argv[3];
    if (argc > 4) message_count = std::atoi(argv[4]);

    std::cout << "========================================\n";
    std::cout << "WSS (WebSocket Secure) Client Example\n";
    std::cout << "========================================\n";
    std::cout << "Host: " << host << "\n";
    std::cout << "Port: " << port << "\n";
    std::cout << "Path: " << path << "\n";
    std::cout << "Messages: " << message_count << "\n";
    std::cout << "========================================\n";

    try {
        Runtime runtime(LoadBalanceStrategy::ROUND_ROBIN, 1, 0);
        runtime.start();

        auto* scheduler = runtime.getNextIOScheduler();
        if (!scheduler) {
            std::cerr << "No IO scheduler available\n";
            return 1;
        }

        scheduler->spawn(wssClientCoroutine(host, port, path, message_count));

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
