#include <cassert>
#include <iostream>
#include <string>
#include <string_view>

#ifdef GALAY_HTTP_SSL_ENABLED
#define private public
#include "galay-http/kernel/websocket/WsWriter.h"
#undef private
#include "galay-ssl/async/SslSocket.h"
#endif

using namespace galay::websocket;

int main() {
#ifdef GALAY_HTTP_SSL_ENABLED
    galay::ssl::SslSocket socket(nullptr);
    WsWriterImpl<galay::ssl::SslSocket> writer(WsWriterSetting::byServer(), socket);

    constexpr std::string_view first_payload = "first-frame";
    writer.prepareSslMessage(WsOpcode::Text, first_payload, true);
    const auto first_expected =
        WsFrameParser::toBytes(WsFrameParser::createTextFrame(std::string(first_payload)), false);

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
        std::cerr << "[T62] zero-byte SSL send after partial progress should complete with send error\n";
        return 1;
    }

    if (failed.result->error().code() != kWsSendError) {
        std::cerr << "[T62] zero-byte SSL send should surface WsSendError\n";
        return 1;
    }

    if (writer.getRemainingBytes() != 0 || writer.sentBytes() != 0) {
        std::cerr << "[T62] failed SSL send should clear buffered steady-state before next frame\n";
        return 1;
    }

    constexpr std::string_view second_payload = "second-frame";
    writer.prepareSslMessage(WsOpcode::Text, second_payload, true);
    const auto second_expected =
        WsFrameParser::toBytes(WsFrameParser::createTextFrame(std::string(second_payload)), false);
    auto next = galay::websocket::detail::WsSslSendMachine<galay::ssl::SslSocket>(&writer).advance();
    assert(next.signal == galay::ssl::SslMachineSignal::kSend);
    assert(next.write_length == second_expected.size());
#endif

    std::cout << "T62-WssWriterSteadyState PASS\n";
    return 0;
}
