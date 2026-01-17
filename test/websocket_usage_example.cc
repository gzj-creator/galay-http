/**
 * @file websocket_usage_example.cc
 * @brief WebSocket 使用示例和说明
 * @details 展示如何使用 WebSocket 连接类
 */

#include <iostream>
#include "galay-http/kernel/http/HttpConn.h"
#include "galay-http/kernel/websocket/WsConn.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/protoc/websocket/WebSocketFrame.h"
#include "galay-http/utils/HttpUtils.h"

using namespace galay::http;
using namespace galay::websocket;

/**
 * @brief WebSocket 使用示例
 *
 * 这个文件展示了如何使用 WebSocket 连接类的基本流程。
 * 注意：这是伪代码示例，实际使用需要配合协程和 IO 调度器。
 */

void example_websocket_upgrade()
{
    std::cout << "=== WebSocket 升级示例 ===" << std::endl;
    std::cout << R"(
// 1. 接收 HTTP 请求并检查是否是 WebSocket 升级请求
HttpRequest request;
// ... 读取 HTTP 请求 ...

if (HttpUtils::isWebSocketUpgrade(request)) {
    // 2. 验证 WebSocket 握手
    auto key = request.getHeader("Sec-WebSocket-Key");
    if (!key.has_value()) {
        // 返回 400 Bad Request
        return;
    }

    // 3. 生成 Sec-WebSocket-Accept
    std::string accept = HttpUtils::generateWebSocketAccept(key.value());

    // 4. 发送 101 Switching Protocols 响应
    HttpResponse response(101, "Switching Protocols");
    response.setHeader("Upgrade", "websocket");
    response.setHeader("Connection", "Upgrade");
    response.setHeader("Sec-WebSocket-Accept", accept);

    // co_await writer.sendResponse(response);

    // 5. 升级到 WebSocket 连接
    auto ws_conn = http_conn.upgrade<WsConn>(
        WsReaderSetting(),
        WsWriterSetting(false),  // 服务器端不使用掩码
        true  // is_server
    );

    // 6. 使用 WebSocket 连接
    auto reader = ws_conn->getReader();
    auto writer = ws_conn->getWriter();

    // 7. 读取和发送 WebSocket 消息
    WsFrame frame;
    // auto result = co_await reader.getFrame(frame);

    // 8. 根据帧类型处理
    switch (frame.header.opcode) {
        case WsOpcode::Text:
            // 处理文本消息
            // co_await writer.sendText(frame.payload);
            break;
        case WsOpcode::Binary:
            // 处理二进制消息
            // co_await writer.sendBinary(frame.payload);
            break;
        case WsOpcode::Ping:
            // 响应 Pong
            // co_await writer.sendPong(frame.payload);
            break;
        case WsOpcode::Close:
            // 关闭连接
            // co_await writer.sendClose(WsCloseCode::Normal);
            break;
    }
}
)" << std::endl;
}

void example_websocket_client()
{
    std::cout << "\n=== WebSocket 客户端示例 ===" << std::endl;
    std::cout << R"(
// 1. 建立 TCP 连接
TcpSocket socket(scheduler);
// co_await socket.connect(host, port);

// 2. 发送 WebSocket 握手请求
HttpRequest request;
request.setMethod("GET");
request.setPath("/");
request.setHeader("Host", "example.com");
request.setHeader("Upgrade", "websocket");
request.setHeader("Connection", "Upgrade");
request.setHeader("Sec-WebSocket-Version", "13");

std::string key = HttpUtils::generateWebSocketKey();
request.setHeader("Sec-WebSocket-Key", key);

// 发送请求并接收响应
// ...

// 3. 验证握手响应
HttpResponse response;
// ... 读取响应 ...

if (response.getStatusCode() == 101) {
    auto accept = response.getHeader("Sec-WebSocket-Accept");
    std::string expected = HttpUtils::generateWebSocketAccept(key);

    if (accept.value() == expected) {
        // 4. 创建 WebSocket 连接
        auto ws_conn = std::make_unique<WsConn>(
            std::move(socket),
            std::move(ring_buffer),
            WsReaderSetting(),
            WsWriterSetting(true),  // 客户端使用掩码
            false  // is_client
        );

        // 5. 使用 WebSocket 连接
        auto reader = ws_conn->getReader();
        auto writer = ws_conn->getWriter();

        // 发送消息
        // co_await writer.sendText("Hello Server!");

        // 接收消息
        WsFrame frame;
        // co_await reader.getFrame(frame);
    }
}
)" << std::endl;
}

void example_websocket_message_handling()
{
    std::cout << "\n=== WebSocket 消息处理示例 ===" << std::endl;
    std::cout << R"(
// 读取完整消息（自动处理分片）
std::string message;
WsOpcode opcode;
// auto result = co_await reader.getMessage(message, opcode);

if (result.has_value() && result.value()) {
    // 消息完整接收
    if (opcode == WsOpcode::Text) {
        std::cout << "Received text: " << message << std::endl;
    } else if (opcode == WsOpcode::Binary) {
        std::cout << "Received binary: " << message.size() << " bytes" << std::endl;
    }
}

// 发送大消息（自动分片）
std::string large_data(1024 * 1024, 'A');  // 1MB
// co_await writer.sendText(large_data);

// 手动分片发送
std::string part1 = "Hello ";
std::string part2 = "World!";

// co_await writer.sendText(part1, false);  // FIN=0
// co_await writer.sendText(part2, true);   // FIN=1
)" << std::endl;
}

void example_websocket_control_frames()
{
    std::cout << "\n=== WebSocket 控制帧示例 ===" << std::endl;
    std::cout << R"(
// 发送 Ping
// co_await writer.sendPing("ping");

// 发送 Pong
// co_await writer.sendPong("pong");

// 发送 Close
// co_await writer.sendClose(WsCloseCode::Normal, "Goodbye");

// 处理控制帧
WsFrame frame;
// co_await reader.getFrame(frame);

if (frame.header.opcode == WsOpcode::Ping) {
    // 自动响应 Pong
    // co_await writer.sendPong(frame.payload);
}

if (frame.header.opcode == WsOpcode::Close) {
    // 提取关闭码和原因
    if (frame.payload.size() >= 2) {
        uint16_t code = (static_cast<uint8_t>(frame.payload[0]) << 8) |
                        static_cast<uint8_t>(frame.payload[1]);
        std::string reason = frame.payload.substr(2);
        std::cout << "Close code: " << code << ", reason: " << reason << std::endl;
    }

    // 响应关闭
    // co_await writer.sendClose(WsCloseCode::Normal);
}
)" << std::endl;
}

void example_websocket_error_handling()
{
    std::cout << "\n=== WebSocket 错误处理示例 ===" << std::endl;
    std::cout << R"(
WsFrame frame;
auto result = co_await reader.getFrame(frame);

if (!result.has_value()) {
    WsError error = result.error();

    std::cout << "Error: " << error.message() << std::endl;

    // 根据错误类型处理
    switch (error.code()) {
        case kWsIncomplete:
            // 数据不完整，继续读取
            break;

        case kWsProtocolError:
        case kWsInvalidFrame:
            // 协议错误，发送关闭帧
            // co_await writer.sendClose(error.toCloseCode(), error.message());
            break;

        case kWsConnectionClosed:
            // 连接已关闭
            break;

        case kWsMessageTooLarge:
            // 消息过大
            // co_await writer.sendClose(WsCloseCode::MessageTooBig);
            break;
    }
}
)" << std::endl;
}

void example_websocket_configuration()
{
    std::cout << "\n=== WebSocket 配置示例 ===" << std::endl;
    std::cout << R"(
// 服务器端配置
WsReaderSetting reader_setting;
reader_setting.max_frame_size = 10 * 1024 * 1024;      // 10MB
reader_setting.max_message_size = 100 * 1024 * 1024;   // 100MB
reader_setting.auto_fragment = true;

WsWriterSetting writer_setting(false);  // 服务器端
writer_setting.max_frame_size = 10 * 1024 * 1024;
writer_setting.auto_fragment = true;
writer_setting.use_mask = false;  // 服务器端不使用掩码

// 客户端配置
WsWriterSetting client_writer_setting(true);  // 客户端
client_writer_setting.use_mask = true;  // 客户端必须使用掩码
)" << std::endl;
}

int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << "WebSocket 使用示例和说明" << std::endl;
    std::cout << "========================================" << std::endl;

    example_websocket_upgrade();
    example_websocket_client();
    example_websocket_message_handling();
    example_websocket_control_frames();
    example_websocket_error_handling();
    example_websocket_configuration();

    std::cout << "\n========================================" << std::endl;
    std::cout << "主要特性：" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "✓ 完整的 RFC 6455 WebSocket 协议支持" << std::endl;
    std::cout << "✓ 自动处理分片消息" << std::endl;
    std::cout << "✓ 支持文本和二进制消息" << std::endl;
    std::cout << "✓ 完整的控制帧支持 (Ping/Pong/Close)" << std::endl;
    std::cout << "✓ 严格的 UTF-8 验证" << std::endl;
    std::cout << "✓ 协程友好的异步接口" << std::endl;
    std::cout << "✓ 零拷贝设计" << std::endl;
    std::cout << "✓ 完整的错误处理" << std::endl;
    std::cout << "✓ 可配置的消息大小限制" << std::endl;
    std::cout << "✓ HTTP 到 WebSocket 无缝升级" << std::endl;
    std::cout << "========================================" << std::endl;

    std::cout << "\n📚 API 文档：" << std::endl;
    std::cout << "  - WsConn: WebSocket 连接类" << std::endl;
    std::cout << "  - WsReader: WebSocket 读取器" << std::endl;
    std::cout << "  - WsWriter: WebSocket 写入器" << std::endl;
    std::cout << "  - WsFrame: WebSocket 帧结构" << std::endl;
    std::cout << "  - WsFrameParser: 帧解析器" << std::endl;
    std::cout << "  - HttpConn::upgrade(): 协议升级方法" << std::endl;

    std::cout << "\n✅ 所有示例代码已展示完成！" << std::endl;

    return 0;
}
