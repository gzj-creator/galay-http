/**
 * @file test_websocket_frame.cc
 * @brief WebSocket Frame Parser 单元测试
 */

#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>
#include "galay-http/protoc/websocket/WebSocketFrame.h"

using namespace galay::websocket;

void test_frame_parsing_text() {
    std::cout << "Testing text frame parsing..." << std::endl;

    // 构造一个简单的文本帧: FIN=1, opcode=Text, mask=1, payload="Hello"
    std::string input;
    input.push_back(0x81);  // FIN=1, opcode=1 (Text)
    input.push_back(0x85);  // MASK=1, payload_len=5
    // 掩码密钥
    uint8_t mask_key[4] = {0x12, 0x34, 0x56, 0x78};
    for (int i = 0; i < 4; ++i) {
        input.push_back(mask_key[i]);
    }
    // Payload "Hello" 经过掩码
    std::string payload = "Hello";
    for (size_t i = 0; i < payload.size(); ++i) {
        input.push_back(payload[i] ^ mask_key[i % 4]);
    }

    std::vector<iovec> iovecs(1);
    iovecs[0].iov_base = const_cast<char*>(input.data());
    iovecs[0].iov_len = input.size();

    WsFrame frame;
    auto result = WsFrameParser::fromIOVec(iovecs, frame, true);

    assert(result.has_value());
    assert(result.value() == input.size());
    assert(frame.header.fin == true);
    assert(frame.header.opcode == WsOpcode::Text);
    assert(frame.header.mask == true);
    assert(frame.payload == "Hello");

    std::cout << "  ✓ Text frame parsed: \"" << frame.payload << "\"" << std::endl;
}

void test_frame_parsing_binary() {
    std::cout << "\nTesting binary frame parsing..." << std::endl;

    // 构造二进制帧
    std::string input;
    input.push_back(0x82);  // FIN=1, opcode=2 (Binary)
    input.push_back(0x84);  // MASK=1, payload_len=4
    // 掩码密钥
    uint8_t mask_key[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    for (int i = 0; i < 4; ++i) {
        input.push_back(mask_key[i]);
    }
    // Payload 经过掩码
    std::string payload = "\x01\x02\x03\x04";
    for (size_t i = 0; i < payload.size(); ++i) {
        input.push_back(payload[i] ^ mask_key[i % 4]);
    }

    std::vector<iovec> iovecs(1);
    iovecs[0].iov_base = const_cast<char*>(input.data());
    iovecs[0].iov_len = input.size();

    WsFrame frame;
    auto result = WsFrameParser::fromIOVec(iovecs, frame, true);

    assert(result.has_value());
    assert(frame.header.opcode == WsOpcode::Binary);
    assert(frame.payload == payload);

    std::cout << "  ✓ Binary frame parsed: " << frame.payload.size() << " bytes" << std::endl;
}

void test_frame_parsing_extended_length_16() {
    std::cout << "\nTesting extended length (16-bit) frame..." << std::endl;

    // 构造一个126字节的payload
    std::string payload(126, 'A');

    std::string input;
    input.push_back(0x81);  // FIN=1, opcode=1 (Text)
    input.push_back(0xFE);  // MASK=1, payload_len=126 (使用16位扩展长度)
    // 16位扩展长度
    input.push_back(0x00);
    input.push_back(0x7E);  // 126
    // 掩码密钥
    uint8_t mask_key[4] = {0x11, 0x22, 0x33, 0x44};
    for (int i = 0; i < 4; ++i) {
        input.push_back(mask_key[i]);
    }
    // Payload 经过掩码
    for (size_t i = 0; i < payload.size(); ++i) {
        input.push_back(payload[i] ^ mask_key[i % 4]);
    }

    std::vector<iovec> iovecs(1);
    iovecs[0].iov_base = const_cast<char*>(input.data());
    iovecs[0].iov_len = input.size();

    WsFrame frame;
    auto result = WsFrameParser::fromIOVec(iovecs, frame, true);

    assert(result.has_value());
    assert(frame.header.payload_length == 126);
    assert(frame.payload.size() == 126);

    std::cout << "  ✓ Extended length (16-bit) frame parsed: " << frame.payload.size() << " bytes" << std::endl;
}

void test_frame_parsing_extended_length_64() {
    std::cout << "\nTesting extended length (64-bit) frame..." << std::endl;

    // 构造一个65536字节的payload
    size_t payload_size = 65536;
    std::string payload(payload_size, 'B');

    std::string input;
    input.push_back(0x82);  // FIN=1, opcode=2 (Binary)
    input.push_back(0xFF);  // MASK=1, payload_len=127 (使用64位扩展长度)
    // 64位扩展长度
    for (int i = 7; i >= 0; --i) {
        input.push_back((payload_size >> (i * 8)) & 0xFF);
    }
    // 掩码密钥
    uint8_t mask_key[4] = {0x55, 0x66, 0x77, 0x88};
    for (int i = 0; i < 4; ++i) {
        input.push_back(mask_key[i]);
    }
    // Payload 经过掩码
    for (size_t i = 0; i < payload.size(); ++i) {
        input.push_back(payload[i] ^ mask_key[i % 4]);
    }

    std::vector<iovec> iovecs(1);
    iovecs[0].iov_base = const_cast<char*>(input.data());
    iovecs[0].iov_len = input.size();

    WsFrame frame;
    auto result = WsFrameParser::fromIOVec(iovecs, frame, true);

    assert(result.has_value());
    assert(frame.header.payload_length == payload_size);
    assert(frame.payload.size() == payload_size);

    std::cout << "  ✓ Extended length (64-bit) frame parsed: " << frame.payload.size() << " bytes" << std::endl;
}

void test_frame_parsing_control_frames() {
    std::cout << "\nTesting control frames..." << std::endl;

    // 测试 Ping 帧
    {
        std::string input;
        input.push_back(0x89);  // FIN=1, opcode=9 (Ping)
        input.push_back(0x84);  // MASK=1, payload_len=4
        uint8_t mask_key[4] = {0x01, 0x02, 0x03, 0x04};
        for (int i = 0; i < 4; ++i) {
            input.push_back(mask_key[i]);
        }
        std::string payload = "ping";
        for (size_t i = 0; i < payload.size(); ++i) {
            input.push_back(payload[i] ^ mask_key[i % 4]);
        }

        std::vector<iovec> iovecs(1);
        iovecs[0].iov_base = const_cast<char*>(input.data());
        iovecs[0].iov_len = input.size();

        WsFrame frame;
        auto result = WsFrameParser::fromIOVec(iovecs, frame, true);

        assert(result.has_value());
        assert(frame.header.opcode == WsOpcode::Ping);
        assert(frame.payload == "ping");
        std::cout << "  ✓ Ping frame parsed" << std::endl;
    }

    // 测试 Pong 帧
    {
        std::string input;
        input.push_back(0x8A);  // FIN=1, opcode=10 (Pong)
        input.push_back(0x84);  // MASK=1, payload_len=4
        uint8_t mask_key[4] = {0x05, 0x06, 0x07, 0x08};
        for (int i = 0; i < 4; ++i) {
            input.push_back(mask_key[i]);
        }
        std::string payload = "pong";
        for (size_t i = 0; i < payload.size(); ++i) {
            input.push_back(payload[i] ^ mask_key[i % 4]);
        }

        std::vector<iovec> iovecs(1);
        iovecs[0].iov_base = const_cast<char*>(input.data());
        iovecs[0].iov_len = input.size();

        WsFrame frame;
        auto result = WsFrameParser::fromIOVec(iovecs, frame, true);

        assert(result.has_value());
        assert(frame.header.opcode == WsOpcode::Pong);
        assert(frame.payload == "pong");
        std::cout << "  ✓ Pong frame parsed" << std::endl;
    }

    // 测试 Close 帧
    {
        std::string input;
        input.push_back(0x88);  // FIN=1, opcode=8 (Close)
        input.push_back(0x82);  // MASK=1, payload_len=2
        uint8_t mask_key[4] = {0x09, 0x0A, 0x0B, 0x0C};
        for (int i = 0; i < 4; ++i) {
            input.push_back(mask_key[i]);
        }
        // Close code 1000 (Normal)
        uint16_t close_code = 1000;
        input.push_back(((close_code >> 8) & 0xFF) ^ mask_key[0]);
        input.push_back((close_code & 0xFF) ^ mask_key[1]);

        std::vector<iovec> iovecs(1);
        iovecs[0].iov_base = const_cast<char*>(input.data());
        iovecs[0].iov_len = input.size();

        WsFrame frame;
        auto result = WsFrameParser::fromIOVec(iovecs, frame, true);

        assert(result.has_value());
        assert(frame.header.opcode == WsOpcode::Close);
        assert(frame.payload.size() == 2);
        std::cout << "  ✓ Close frame parsed" << std::endl;
    }
}

void test_frame_parsing_fragmented() {
    std::cout << "\nTesting fragmented frames..." << std::endl;

    // 第一个分片 (FIN=0)
    std::string input1;
    input1.push_back(0x01);  // FIN=0, opcode=1 (Text)
    input1.push_back(0x85);  // MASK=1, payload_len=5
    uint8_t mask_key1[4] = {0x11, 0x11, 0x11, 0x11};
    for (int i = 0; i < 4; ++i) {
        input1.push_back(mask_key1[i]);
    }
    std::string payload1 = "Hello";
    for (size_t i = 0; i < payload1.size(); ++i) {
        input1.push_back(payload1[i] ^ mask_key1[i % 4]);
    }

    std::vector<iovec> iovecs1(1);
    iovecs1[0].iov_base = const_cast<char*>(input1.data());
    iovecs1[0].iov_len = input1.size();

    WsFrame frame1;
    auto result1 = WsFrameParser::fromIOVec(iovecs1, frame1, true);

    assert(result1.has_value());
    assert(frame1.header.fin == false);
    assert(frame1.header.opcode == WsOpcode::Text);
    assert(frame1.payload == "Hello");

    // 第二个分片 (FIN=1, opcode=Continuation)
    std::string input2;
    input2.push_back(0x80);  // FIN=1, opcode=0 (Continuation)
    input2.push_back(0x86);  // MASK=1, payload_len=6
    uint8_t mask_key2[4] = {0x22, 0x22, 0x22, 0x22};
    for (int i = 0; i < 4; ++i) {
        input2.push_back(mask_key2[i]);
    }
    std::string payload2 = " World";
    for (size_t i = 0; i < payload2.size(); ++i) {
        input2.push_back(payload2[i] ^ mask_key2[i % 4]);
    }

    std::vector<iovec> iovecs2(1);
    iovecs2[0].iov_base = const_cast<char*>(input2.data());
    iovecs2[0].iov_len = input2.size();

    WsFrame frame2;
    auto result2 = WsFrameParser::fromIOVec(iovecs2, frame2, true);

    assert(result2.has_value());
    assert(frame2.header.fin == true);
    assert(frame2.header.opcode == WsOpcode::Continuation);
    assert(frame2.payload == " World");

    std::cout << "  ✓ Fragmented frames parsed: \"" << frame1.payload << "\" + \"" << frame2.payload << "\"" << std::endl;
}

void test_frame_parsing_errors() {
    std::cout << "\nTesting error cases..." << std::endl;

    // 测试1: 数据不完整
    {
        std::string input;
        input.push_back(0x81);  // 只有第一个字节

        std::vector<iovec> iovecs(1);
        iovecs[0].iov_base = const_cast<char*>(input.data());
        iovecs[0].iov_len = input.size();

        WsFrame frame;
        auto result = WsFrameParser::fromIOVec(iovecs, frame, true);

        assert(!result.has_value());
        assert(result.error().code() == kWsIncomplete);
        std::cout << "  ✓ Incomplete data detected" << std::endl;
    }

    // 测试2: 服务器端要求客户端必须使用掩码
    {
        std::string input;
        input.push_back(0x81);  // FIN=1, opcode=1 (Text)
        input.push_back(0x05);  // MASK=0, payload_len=5
        input += "Hello";

        std::vector<iovec> iovecs(1);
        iovecs[0].iov_base = const_cast<char*>(input.data());
        iovecs[0].iov_len = input.size();

        WsFrame frame;
        auto result = WsFrameParser::fromIOVec(iovecs, frame, true);

        assert(!result.has_value());
        assert(result.error().code() == kWsMaskRequired);
        std::cout << "  ✓ Mask required error detected" << std::endl;
    }

    // 测试3: 控制帧不能分片
    {
        std::string input;
        input.push_back(0x08);  // FIN=0, opcode=8 (Close) - 错误！
        input.push_back(0x80);  // MASK=1, payload_len=0

        std::vector<iovec> iovecs(1);
        iovecs[0].iov_base = const_cast<char*>(input.data());
        iovecs[0].iov_len = input.size();

        WsFrame frame;
        auto result = WsFrameParser::fromIOVec(iovecs, frame, true);

        assert(!result.has_value());
        assert(result.error().code() == kWsControlFrameFragmented);
        std::cout << "  ✓ Control frame fragmented error detected" << std::endl;
    }

    // 测试4: 保留位被设置
    {
        std::string input;
        input.push_back(0xC1);  // FIN=1, RSV1=1, opcode=1 - 错误！
        input.push_back(0x80);  // MASK=1, payload_len=0

        std::vector<iovec> iovecs(1);
        iovecs[0].iov_base = const_cast<char*>(input.data());
        iovecs[0].iov_len = input.size();

        WsFrame frame;
        auto result = WsFrameParser::fromIOVec(iovecs, frame, true);

        assert(!result.has_value());
        assert(result.error().code() == kWsReservedBitsSet);
        std::cout << "  ✓ Reserved bits set error detected" << std::endl;
    }
}

void test_frame_encoding() {
    std::cout << "\nTesting frame encoding..." << std::endl;

    // 测试1: 编码文本帧（不使用掩码）
    {
        WsFrame frame = WsFrameParser::createTextFrame("Hello");
        std::string encoded = WsFrameParser::toBytes(frame, false);

        assert(encoded.size() == 2 + 5);  // 2字节头 + 5字节payload
        assert((uint8_t)encoded[0] == 0x81);  // FIN=1, opcode=1
        assert((uint8_t)encoded[1] == 0x05);  // MASK=0, len=5
        assert(encoded.substr(2) == "Hello");

        std::cout << "  ✓ Text frame encoded (no mask): " << encoded.size() << " bytes" << std::endl;
    }

    // 测试2: 编码二进制帧（使用掩码）
    {
        WsFrame frame = WsFrameParser::createBinaryFrame("Data");
        std::string encoded = WsFrameParser::toBytes(frame, true);

        assert(encoded.size() == 2 + 4 + 4);  // 2字节头 + 4字节掩码 + 4字节payload
        assert((uint8_t)encoded[0] == 0x82);  // FIN=1, opcode=2
        assert(((uint8_t)encoded[1] & 0x80) == 0x80);  // MASK=1

        std::cout << "  ✓ Binary frame encoded (with mask): " << encoded.size() << " bytes" << std::endl;
    }

    // 测试3: 编码Ping帧
    {
        WsFrame frame = WsFrameParser::createPingFrame("ping");
        std::string encoded = WsFrameParser::toBytes(frame, false);

        assert((uint8_t)encoded[0] == 0x89);  // FIN=1, opcode=9
        std::cout << "  ✓ Ping frame encoded" << std::endl;
    }

    // 测试4: 编码Pong帧
    {
        WsFrame frame = WsFrameParser::createPongFrame("pong");
        std::string encoded = WsFrameParser::toBytes(frame, false);

        assert((uint8_t)encoded[0] == 0x8A);  // FIN=1, opcode=10
        std::cout << "  ✓ Pong frame encoded" << std::endl;
    }

    // 测试5: 编码Close帧
    {
        WsFrame frame = WsFrameParser::createCloseFrame(WsCloseCode::Normal, "Goodbye");
        std::string encoded = WsFrameParser::toBytes(frame, false);

        assert((uint8_t)encoded[0] == 0x88);  // FIN=1, opcode=8
        std::cout << "  ✓ Close frame encoded" << std::endl;
    }
}

void test_frame_roundtrip() {
    std::cout << "\nTesting frame roundtrip (encode -> decode)..." << std::endl;

    // 测试文本帧往返
    {
        std::string original_text = "Hello WebSocket!";
        WsFrame original_frame = WsFrameParser::createTextFrame(original_text);

        // 编码（使用掩码）
        std::string encoded = WsFrameParser::toBytes(original_frame, true);

        // 解码
        std::vector<iovec> iovecs(1);
        iovecs[0].iov_base = const_cast<char*>(encoded.data());
        iovecs[0].iov_len = encoded.size();

        WsFrame decoded_frame;
        auto result = WsFrameParser::fromIOVec(iovecs, decoded_frame, true);

        assert(result.has_value());
        assert(decoded_frame.header.opcode == WsOpcode::Text);
        assert(decoded_frame.payload == original_text);

        std::cout << "  ✓ Text frame roundtrip: \"" << decoded_frame.payload << "\"" << std::endl;
    }

    // 测试二进制帧往返
    {
        std::string original_data = "\x01\x02\x03\x04\x05";
        WsFrame original_frame = WsFrameParser::createBinaryFrame(original_data);

        std::string encoded = WsFrameParser::toBytes(original_frame, true);

        std::vector<iovec> iovecs(1);
        iovecs[0].iov_base = const_cast<char*>(encoded.data());
        iovecs[0].iov_len = encoded.size();

        WsFrame decoded_frame;
        auto result = WsFrameParser::fromIOVec(iovecs, decoded_frame, true);

        assert(result.has_value());
        assert(decoded_frame.header.opcode == WsOpcode::Binary);
        assert(decoded_frame.payload == original_data);

        std::cout << "  ✓ Binary frame roundtrip: " << decoded_frame.payload.size() << " bytes" << std::endl;
    }
}

void test_utf8_validation() {
    std::cout << "\nTesting UTF-8 validation..." << std::endl;

    // 有效的UTF-8
    assert(WsFrameParser::isValidUtf8("Hello"));
    assert(WsFrameParser::isValidUtf8("你好世界"));
    assert(WsFrameParser::isValidUtf8("Hello 世界 🌍"));
    std::cout << "  ✓ Valid UTF-8 strings accepted" << std::endl;

    // 无效的UTF-8
    assert(!WsFrameParser::isValidUtf8(std::string("\xFF\xFE")));
    assert(!WsFrameParser::isValidUtf8(std::string("\xC0\x80")));  // 过长编码
    std::cout << "  ✓ Invalid UTF-8 strings rejected" << std::endl;
}

void test_cross_iovec_parsing() {
    std::cout << "\nTesting cross-iovec frame parsing..." << std::endl;

    // 构造一个跨越多个iovec的帧
    std::string part1;
    part1.push_back(0x81);  // FIN=1, opcode=1
    part1.push_back(0x85);  // MASK=1, len=5

    std::string part2;
    uint8_t mask_key[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    for (int i = 0; i < 4; ++i) {
        part2.push_back(mask_key[i]);
    }

    std::string part3;
    std::string payload = "Hello";
    for (size_t i = 0; i < payload.size(); ++i) {
        part3.push_back(payload[i] ^ mask_key[i % 4]);
    }

    std::vector<iovec> iovecs(3);
    iovecs[0].iov_base = const_cast<char*>(part1.data());
    iovecs[0].iov_len = part1.size();
    iovecs[1].iov_base = const_cast<char*>(part2.data());
    iovecs[1].iov_len = part2.size();
    iovecs[2].iov_base = const_cast<char*>(part3.data());
    iovecs[2].iov_len = part3.size();

    WsFrame frame;
    auto result = WsFrameParser::fromIOVec(iovecs, frame, true);

    assert(result.has_value());
    assert(frame.payload == "Hello");

    std::cout << "  ✓ Cross-iovec frame parsed: \"" << frame.payload << "\"" << std::endl;
}

int main() {
    std::cout << "=== WebSocket Frame Parser Unit Tests ===" << std::endl;

    try {
        test_frame_parsing_text();
        test_frame_parsing_binary();
        test_frame_parsing_extended_length_16();
        test_frame_parsing_extended_length_64();
        test_frame_parsing_control_frames();
        test_frame_parsing_fragmented();
        test_frame_parsing_errors();
        test_frame_encoding();
        test_frame_roundtrip();
        test_utf8_validation();
        test_cross_iovec_parsing();

        std::cout << "\n✅ All tests passed!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ Test failed: " << e.what() << std::endl;
        return 1;
    }
}
