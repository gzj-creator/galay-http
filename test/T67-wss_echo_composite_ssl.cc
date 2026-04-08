#include <cstring>
#include <iostream>
#include <string>

#ifdef GALAY_HTTP_SSL_ENABLED
#define private public
#include "galay-http/kernel/websocket/WsConn.h"
#undef private
#include "galay-http/protoc/websocket/WebSocketFrame.h"
#include "galay-kernel/common/Bytes.h"
#include "galay-kernel/common/Buffer.h"
#include "galay-ssl/async/SslSocket.h"
#endif

namespace {

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[T67] " << message << "\n";
        return false;
    }
    return true;
}

std::string encodeMaskedFrame(galay::websocket::WsOpcode opcode, std::string payload, bool fin = true) {
    galay::websocket::WsFrame frame(opcode, std::move(payload), fin);
    std::string encoded;
    galay::websocket::WsFrameParser::encodeInto(encoded, frame, true);
    return encoded;
}

galay::kernel::RingBuffer makeWrappedFrameBuffer(std::string_view encoded, size_t capacity = 64, size_t prefix = 40) {
    galay::kernel::RingBuffer ring(capacity);
    std::string head(prefix, 'x');
    if (ring.write(head.data(), head.size()) != head.size()) {
        throw std::runtime_error("failed to seed ring prefix");
    }
    ring.consume(prefix - 10);
    if (ring.write(encoded.data(), encoded.size()) != encoded.size()) {
        throw std::runtime_error("failed to wrap encoded frame into ring");
    }
    ring.consume(10);
    return ring;
}

template<typename MachineT, typename ActionT>
std::string drainSslSend(MachineT& machine, ActionT action) {
    std::string sent;
    while (action.signal == galay::ssl::SslMachineSignal::kSend) {
        sent.append(action.write_buffer, action.write_length);
        machine.onSend(std::expected<size_t, galay::ssl::SslError>(action.write_length));
        action = machine.advance();
    }
    return sent;
}

} // namespace

int main() {
#ifndef GALAY_HTTP_SSL_ENABLED
    std::cout << "T67-WssEchoCompositeSsl SKIP\n";
    return 0;
#else
    using namespace galay::websocket;

    {
        galay::ssl::SslSocket socket(nullptr);
        WssConn conn(std::move(socket), true);
        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        galay::websocket::detail::WsSslEchoMachine<galay::ssl::SslSocket> machine(
            &conn,
            WsReaderSetting(),
            WsWriterSetting::byServer(),
            message,
            opcode);

        const auto first = machine.advance();
        if (!check(first.signal == galay::ssl::SslMachineSignal::kRecv,
                   "ssl composite echo should recv first")) {
            return 1;
        }

        const std::string payload = "hello ssl composite";
        const auto encoded = encodeMaskedFrame(WsOpcode::Text, payload);
        if (!check(first.read_length >= encoded.size(),
                   "ssl composite recv window should fit encoded frame")) {
            return 1;
        }
        std::memcpy(first.read_buffer, encoded.data(), encoded.size());

        machine.onRecv(std::expected<galay::kernel::Bytes, galay::ssl::SslError>(
            galay::kernel::Bytes(first.read_buffer, encoded.size())));

        const auto second = machine.advance();
        if (!check(second.signal == galay::ssl::SslMachineSignal::kSend,
                   "ssl composite echo should send immediately after parse")) {
            return 1;
        }

        const auto expected = WsFrameParser::toBytes(WsFrameParser::createTextFrame(payload), false);
        if (!check(second.write_length == expected.size(),
                   "ssl composite echo send length mismatch")) {
            return 1;
        }
        if (!check(std::string(second.write_buffer, second.write_length) == expected,
                   "ssl composite echo send payload mismatch")) {
            return 1;
        }
        if (!check(conn.m_echo_counters.composite_hits == 1,
                   "ssl composite echo should count one hit")) {
            return 1;
        }
        if (!check(conn.m_echo_counters.ssl_direct_message_hits == 1,
                   "ssl composite echo should use direct message encoding")) {
            return 1;
        }
        if (!check(conn.m_echo_counters.zero_copy_hits == 0,
                   "ssl preserve composite echo should not count zero-copy hit")) {
            return 1;
        }

        machine.onSend(std::expected<size_t, galay::ssl::SslError>(second.write_length));
        const auto third = machine.advance();
        if (!check(third.signal == galay::ssl::SslMachineSignal::kComplete,
                   "ssl composite echo should complete after send")) {
            return 1;
        }
        if (!check(third.result.has_value() && third.result->has_value() && third.result->value(),
                   "ssl composite echo should complete successfully")) {
            return 1;
        }
        if (!check(message == payload, "ssl composite echo should preserve message")) {
            return 1;
        }
    }

    {
        galay::ssl::SslSocket socket(nullptr);
        WssConn conn(std::move(socket), true);
        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        auto echo_op = conn.echoOnceConsume(message, opcode);
        (void) echo_op;
        if (!check(conn.m_echo_counters.composite_awaitables_started == 1,
                   "ssl consume composite path should start one awaitable")) {
            return 1;
        }
    }

    {
        galay::ssl::SslSocket socket(nullptr);
        WssConn conn(std::move(socket), true);
        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        galay::websocket::detail::WsSslEchoMachine<galay::ssl::SslSocket> machine(
            &conn,
            WsReaderSetting(),
            WsWriterSetting::byServer(),
            message,
            opcode,
            false);

        const auto first = machine.advance();
        if (!check(first.signal == galay::ssl::SslMachineSignal::kRecv,
                   "ssl consume composite echo should recv first")) {
            return 1;
        }

        const std::string payload = "consume ssl composite";
        const auto encoded = encodeMaskedFrame(WsOpcode::Text, payload);
        if (!check(first.read_length >= encoded.size(),
                   "ssl consume composite recv window should fit encoded frame")) {
            return 1;
        }
        std::memcpy(first.read_buffer, encoded.data(), encoded.size());
        machine.onRecv(std::expected<galay::kernel::Bytes, galay::ssl::SslError>(
            galay::kernel::Bytes(first.read_buffer, encoded.size())));

        const auto second = machine.advance();
        if (!check(second.signal == galay::ssl::SslMachineSignal::kSend,
                   "ssl consume composite echo should send after parse")) {
            return 1;
        }

        const auto expected = WsFrameParser::toBytes(WsFrameParser::createTextFrame(payload), false);
        auto send_action = second;
        const auto sent = drainSslSend(machine, send_action);
        if (!check(sent == expected,
                   "ssl consume composite send payload mismatch")) {
            return 1;
        }
        if (!check(conn.m_echo_counters.zero_copy_hits == 1,
                   "ssl consume composite echo should count one zero-copy hit")) {
            return 1;
        }
    }

    {
        galay::ssl::SslSocket socket(nullptr);
        const std::string payload = "wrapped ssl zero-copy";
        auto ring = makeWrappedFrameBuffer(encodeMaskedFrame(WsOpcode::Text, payload));
        WssConn conn(std::move(socket), std::move(ring), true);
        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        galay::websocket::detail::WsSslEchoMachine<galay::ssl::SslSocket> machine(
            &conn,
            WsReaderSetting(),
            WsWriterSetting::byServer(),
            message,
            opcode,
            false);

        const auto first = machine.advance();
        if (!check(first.signal == galay::ssl::SslMachineSignal::kSend,
                   "wrapped ssl consume path should send immediately")) {
            return 1;
        }

        const auto expected = WsFrameParser::toBytes(WsFrameParser::createTextFrame(payload), false);
        auto send_action = first;
        const auto sent = drainSslSend(machine, send_action);
        if (!check(sent == expected,
                   "wrapped ssl consume send payload mismatch")) {
            return 1;
        }
        if (!check(conn.m_echo_counters.zero_copy_hits == 1,
                   "wrapped ssl consume path should count one zero-copy hit")) {
            return 1;
        }
    }

    {
        galay::ssl::SslSocket socket(nullptr);
        WssConn conn(std::move(socket), true);
        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        galay::websocket::detail::WsSslEchoMachine<galay::ssl::SslSocket> machine(
            &conn,
            WsReaderSetting(),
            WsWriterSetting::byServer(),
            message,
            opcode);

        const auto first = machine.advance();
        if (!check(first.signal == galay::ssl::SslMachineSignal::kRecv,
                   "ssl composite ping should recv first")) {
            return 1;
        }

        const auto encoded = encodeMaskedFrame(WsOpcode::Ping, "ping");
        std::memcpy(first.read_buffer, encoded.data(), encoded.size());
        machine.onRecv(std::expected<galay::kernel::Bytes, galay::ssl::SslError>(
            galay::kernel::Bytes(first.read_buffer, encoded.size())));

        const auto second = machine.advance();
        if (!check(second.signal == galay::ssl::SslMachineSignal::kComplete,
                   "ssl composite ping should stay on fallback path")) {
            return 1;
        }
        if (!check(opcode == WsOpcode::Ping, "ssl composite ping should surface opcode")) {
            return 1;
        }
        if (!check(conn.m_echo_counters.composite_hits == 0,
                   "ssl composite ping should not count hit")) {
            return 1;
        }
        if (!check(conn.m_echo_counters.composite_fallbacks == 1,
                   "ssl composite ping should count fallback")) {
            return 1;
        }
    }

    std::cout << "T67-WssEchoCompositeSsl PASS\n";
    return 0;
#endif
}
