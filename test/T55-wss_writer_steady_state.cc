#include <cassert>
#include <iostream>
#include <string>
#include <string_view>

#ifdef GALAY_HTTP_SSL_ENABLED
#define private public
#include "galay-http/kernel/websocket/WsWriter.h"
#undef private
#include "galay-http/protoc/websocket/WebSocketFrame.h"
#include "galay-ssl/async/SslSocket.h"
#endif

int main() {
#ifdef GALAY_HTTP_SSL_ENABLED
    using namespace galay::websocket;

    galay::ssl::SslSocket socket(nullptr);
    WsWriterImpl<galay::ssl::SslSocket> writer(WsWriterSetting::byServer(), socket);

    constexpr std::string_view first_payload = "first-frame";
    (void) writer.sendText(std::string(first_payload));
    const auto first_expected =
        WsFrameParser::toBytes(WsFrameParser::createTextFrame(std::string(first_payload)), false);
    if (std::string(writer.bufferData(), writer.getRemainingBytes()) != first_expected) {
        std::cerr << "[T55] ssl text send should encode directly into expected steady-state buffer\n";
        return 1;
    }

    galay::websocket::detail::WsSslSendMachine<galay::ssl::SslSocket> machine(&writer);
    auto first = machine.advance();
    assert(first.signal == galay::ssl::SslMachineSignal::kSend);
    assert(first.write_length == first_expected.size());

    machine.onSend(std::expected<size_t, galay::ssl::SslError>(first.write_length - 2));
    auto resumed = machine.advance();
    assert(resumed.signal == galay::ssl::SslMachineSignal::kSend);
    assert(resumed.write_length == 2);

    machine.onSend(std::expected<size_t, galay::ssl::SslError>(0));
    auto failed = machine.advance();
    if (failed.signal != galay::ssl::SslMachineSignal::kComplete ||
        !failed.result.has_value() ||
        failed.result->has_value()) {
        std::cerr << "[T55] zero-byte SSL send after partial progress should complete with send error\n";
        return 1;
    }

    if (failed.result->error().code() != kWsSendError) {
        std::cerr << "[T55] zero-byte SSL send should surface WsSendError\n";
        return 1;
    }

    if (writer.getRemainingBytes() != 0 || writer.sentBytes() != 0) {
        std::cerr << "[T55] failed SSL send should clear buffered steady-state before next frame\n";
        return 1;
    }

    constexpr std::string_view second_payload = "second-frame";
    (void) writer.sendText(std::string(second_payload));
    const auto second_expected =
        WsFrameParser::toBytes(WsFrameParser::createTextFrame(std::string(second_payload)), false);
    auto next = galay::websocket::detail::WsSslSendMachine<galay::ssl::SslSocket>(&writer).advance();
    assert(next.signal == galay::ssl::SslMachineSignal::kSend);
    assert(next.write_length == second_expected.size());

    WsWriterImpl<galay::ssl::SslSocket> client_writer(WsWriterSetting::byClient(), socket);
    constexpr std::string_view masked_payload = "masked-client-frame";
    (void) client_writer.sendText(std::string(masked_payload));
    iovec masked_iov {
        const_cast<char*>(client_writer.bufferData()),
        client_writer.getRemainingBytes(),
    };
    WsFrame masked_frame;
    auto masked_result = WsFrameParser::fromIOVec(&masked_iov, 1, masked_frame, true);
    if (!masked_result.has_value()) {
        std::cerr << "[T55] masked ssl text send should remain decodable as a client frame\n";
        return 1;
    }
    if (masked_frame.header.opcode != WsOpcode::Text || masked_frame.payload != masked_payload) {
        std::cerr << "[T55] masked ssl text send payload/opcode mismatch\n";
        return 1;
    }
#endif

    std::cout << "T55-WssWriterSteadyState PASS\n";
    return 0;
}
