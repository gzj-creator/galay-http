/**
 * @file bench_websocket.cc
 * @brief WebSocket Frame 编码/解码性能测试
 */

#include <iostream>
#include <chrono>
#include <vector>
#include <random>
#include "galay-http/protoc/websocket/WebSocketFrame.h"

using namespace galay::websocket;
using namespace std::chrono;

void benchmark_frame_encoding_small() {
    std::cout << "=== Small Frame Encoding Benchmark (64 bytes) ===" << std::endl;

    const int iterations = 1000000;
    std::string testData(64, 'A');

    auto start = high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        WsFrame frame = WsFrameParser::createTextFrame(testData);
        auto encoded = WsFrameParser::toBytes(frame, true);
    }

    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start).count();

    std::cout << "  Iterations: " << iterations << std::endl;
    std::cout << "  Time: " << duration << " ms" << std::endl;
    std::cout << "  Throughput: " << (iterations * 1000 / duration) << " ops/sec" << std::endl;
    std::cout << "  Avg time: " << (duration * 1000.0 / iterations) << " μs/op" << std::endl;
}

void benchmark_frame_encoding_medium() {
    std::cout << "\n=== Medium Frame Encoding Benchmark (1KB) ===" << std::endl;

    const int iterations = 500000;
    std::string testData(1024, 'B');

    auto start = high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        WsFrame frame = WsFrameParser::createTextFrame(testData);
        auto encoded = WsFrameParser::toBytes(frame, true);
    }

    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start).count();

    std::cout << "  Iterations: " << iterations << std::endl;
    std::cout << "  Time: " << duration << " ms" << std::endl;
    std::cout << "  Throughput: " << (iterations * 1000 / duration) << " ops/sec" << std::endl;
    std::cout << "  Avg time: " << (duration * 1000.0 / iterations) << " μs/op" << std::endl;
}

void benchmark_frame_encoding_large() {
    std::cout << "\n=== Large Frame Encoding Benchmark (64KB) ===" << std::endl;

    const int iterations = 10000;
    std::string testData(65536, 'C');

    auto start = high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        WsFrame frame = WsFrameParser::createBinaryFrame(testData);
        auto encoded = WsFrameParser::toBytes(frame, true);
    }

    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start).count();

    std::cout << "  Iterations: " << iterations << std::endl;
    std::cout << "  Time: " << duration << " ms" << std::endl;
    std::cout << "  Throughput: " << (iterations * 1000 / duration) << " ops/sec" << std::endl;
    std::cout << "  Avg time: " << (duration * 1000.0 / iterations) << " μs/op" << std::endl;

    // 计算吞吐量 (MB/s)
    double totalMB = (iterations * 65536.0) / (1024 * 1024);
    double seconds = duration / 1000.0;
    std::cout << "  Data throughput: " << (totalMB / seconds) << " MB/s" << std::endl;
}

void benchmark_frame_decoding_small() {
    std::cout << "\n=== Small Frame Decoding Benchmark (64 bytes) ===" << std::endl;

    const int iterations = 1000000;
    std::string testData(64, 'A');

    // 准备编码后的帧
    WsFrame frame = WsFrameParser::createTextFrame(testData);
    std::string encoded = WsFrameParser::toBytes(frame, true);

    std::vector<iovec> iovecs(1);
    iovecs[0].iov_base = const_cast<char*>(encoded.data());
    iovecs[0].iov_len = encoded.size();

    auto start = high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        WsFrame decoded_frame;
        auto result = WsFrameParser::fromIOVec(iovecs, decoded_frame, true);
    }

    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start).count();

    std::cout << "  Iterations: " << iterations << std::endl;
    std::cout << "  Time: " << duration << " ms" << std::endl;
    std::cout << "  Throughput: " << (iterations * 1000 / duration) << " ops/sec" << std::endl;
    std::cout << "  Avg time: " << (duration * 1000.0 / iterations) << " μs/op" << std::endl;
}

void benchmark_frame_decoding_medium() {
    std::cout << "\n=== Medium Frame Decoding Benchmark (1KB) ===" << std::endl;

    const int iterations = 500000;
    std::string testData(1024, 'B');

    WsFrame frame = WsFrameParser::createTextFrame(testData);
    std::string encoded = WsFrameParser::toBytes(frame, true);

    std::vector<iovec> iovecs(1);
    iovecs[0].iov_base = const_cast<char*>(encoded.data());
    iovecs[0].iov_len = encoded.size();

    auto start = high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        WsFrame decoded_frame;
        auto result = WsFrameParser::fromIOVec(iovecs, decoded_frame, true);
    }

    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start).count();

    std::cout << "  Iterations: " << iterations << std::endl;
    std::cout << "  Time: " << duration << " ms" << std::endl;
    std::cout << "  Throughput: " << (iterations * 1000 / duration) << " ops/sec" << std::endl;
    std::cout << "  Avg time: " << (duration * 1000.0 / iterations) << " μs/op" << std::endl;
}

void benchmark_frame_decoding_large() {
    std::cout << "\n=== Large Frame Decoding Benchmark (64KB) ===" << std::endl;

    const int iterations = 10000;
    std::string testData(65536, 'C');

    WsFrame frame = WsFrameParser::createBinaryFrame(testData);
    std::string encoded = WsFrameParser::toBytes(frame, true);

    std::vector<iovec> iovecs(1);
    iovecs[0].iov_base = const_cast<char*>(encoded.data());
    iovecs[0].iov_len = encoded.size();

    auto start = high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        WsFrame decoded_frame;
        auto result = WsFrameParser::fromIOVec(iovecs, decoded_frame, true);
    }

    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start).count();

    std::cout << "  Iterations: " << iterations << std::endl;
    std::cout << "  Time: " << duration << " ms" << std::endl;
    std::cout << "  Throughput: " << (iterations * 1000 / duration) << " ops/sec" << std::endl;
    std::cout << "  Avg time: " << (duration * 1000.0 / iterations) << " μs/op" << std::endl;

    // 计算吞吐量 (MB/s)
    double totalMB = (iterations * 65536.0) / (1024 * 1024);
    double seconds = duration / 1000.0;
    std::cout << "  Data throughput: " << (totalMB / seconds) << " MB/s" << std::endl;
}

void benchmark_frame_roundtrip() {
    std::cout << "\n=== Frame Roundtrip Benchmark (encode + decode, 1KB) ===" << std::endl;

    const int iterations = 200000;
    std::string testData(1024, 'D');

    auto start = high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        // 编码
        WsFrame frame = WsFrameParser::createTextFrame(testData);
        std::string encoded = WsFrameParser::toBytes(frame, true);

        // 解码
        std::vector<iovec> iovecs(1);
        iovecs[0].iov_base = const_cast<char*>(encoded.data());
        iovecs[0].iov_len = encoded.size();

        WsFrame decoded_frame;
        auto result = WsFrameParser::fromIOVec(iovecs, decoded_frame, true);
    }

    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start).count();

    std::cout << "  Iterations: " << iterations << std::endl;
    std::cout << "  Time: " << duration << " ms" << std::endl;
    std::cout << "  Throughput: " << (iterations * 1000 / duration) << " ops/sec" << std::endl;
    std::cout << "  Avg time: " << (duration * 1000.0 / iterations) << " μs/op" << std::endl;
}

void benchmark_control_frames() {
    std::cout << "\n=== Control Frames Benchmark (Ping/Pong/Close) ===" << std::endl;

    const int iterations = 1000000;

    auto start = high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        // Ping
        WsFrame ping = WsFrameParser::createPingFrame("ping");
        auto ping_encoded = WsFrameParser::toBytes(ping, true);

        // Pong
        WsFrame pong = WsFrameParser::createPongFrame("pong");
        auto pong_encoded = WsFrameParser::toBytes(pong, true);

        // Close
        WsFrame close = WsFrameParser::createCloseFrame(WsCloseCode::Normal);
        auto close_encoded = WsFrameParser::toBytes(close, true);
    }

    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start).count();

    std::cout << "  Iterations: " << iterations << " (x3 frames)" << std::endl;
    std::cout << "  Time: " << duration << " ms" << std::endl;
    std::cout << "  Throughput: " << (iterations * 3 * 1000 / duration) << " ops/sec" << std::endl;
    std::cout << "  Avg time: " << (duration * 1000.0 / (iterations * 3)) << " μs/op" << std::endl;
}

void benchmark_masking() {
    std::cout << "\n=== Masking Performance Benchmark (1KB) ===" << std::endl;

    const int iterations = 500000;
    std::string testData(1024, 'E');
    uint8_t mask_key[4] = {0x12, 0x34, 0x56, 0x78};

    auto start = high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        std::string data = testData;
        WsFrameParser::applyMask(data, mask_key);
    }

    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start).count();

    std::cout << "  Iterations: " << iterations << std::endl;
    std::cout << "  Time: " << duration << " ms" << std::endl;
    std::cout << "  Throughput: " << (iterations * 1000 / duration) << " ops/sec" << std::endl;
    std::cout << "  Avg time: " << (duration * 1000.0 / iterations) << " μs/op" << std::endl;

    // 计算吞吐量 (MB/s)
    double totalMB = (iterations * 1024.0) / (1024 * 1024);
    double seconds = duration / 1000.0;
    std::cout << "  Data throughput: " << (totalMB / seconds) << " MB/s" << std::endl;
}

void benchmark_utf8_validation() {
    std::cout << "\n=== UTF-8 Validation Benchmark ===" << std::endl;

    const int iterations = 500000;

    // ASCII 文本
    std::string ascii_text = "Hello World! This is a test message for UTF-8 validation benchmark.";

    // UTF-8 文本（包含中文）
    std::string utf8_text = "你好世界！这是一个UTF-8验证性能测试消息。Hello World!";

    // ASCII 测试
    {
        auto start = high_resolution_clock::now();
        for (int i = 0; i < iterations; ++i) {
            WsFrameParser::isValidUtf8(ascii_text);
        }
        auto end = high_resolution_clock::now();
        auto duration = duration_cast<milliseconds>(end - start).count();

        std::cout << "  ASCII text (" << ascii_text.size() << " bytes):" << std::endl;
        std::cout << "    Iterations: " << iterations << std::endl;
        std::cout << "    Time: " << duration << " ms" << std::endl;
        std::cout << "    Throughput: " << (iterations * 1000 / duration) << " ops/sec" << std::endl;
    }

    // UTF-8 测试
    {
        auto start = high_resolution_clock::now();
        for (int i = 0; i < iterations; ++i) {
            WsFrameParser::isValidUtf8(utf8_text);
        }
        auto end = high_resolution_clock::now();
        auto duration = duration_cast<milliseconds>(end - start).count();

        std::cout << "  UTF-8 text (" << utf8_text.size() << " bytes):" << std::endl;
        std::cout << "    Iterations: " << iterations << std::endl;
        std::cout << "    Time: " << duration << " ms" << std::endl;
        std::cout << "    Throughput: " << (iterations * 1000 / duration) << " ops/sec" << std::endl;
    }
}

void benchmark_fragmented_frames() {
    std::cout << "\n=== Fragmented Frames Benchmark ===" << std::endl;

    const int iterations = 200000;
    std::string data1 = "Hello ";
    std::string data2 = "World!";

    auto start = high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        // 第一个分片
        WsFrame frame1(WsOpcode::Text, data1, false);
        auto encoded1 = WsFrameParser::toBytes(frame1, true);

        // 第二个分片
        WsFrame frame2(WsOpcode::Continuation, data2, true);
        auto encoded2 = WsFrameParser::toBytes(frame2, true);

        // 解码第一个分片
        std::vector<iovec> iovecs1(1);
        iovecs1[0].iov_base = const_cast<char*>(encoded1.data());
        iovecs1[0].iov_len = encoded1.size();
        WsFrame decoded1;
        WsFrameParser::fromIOVec(iovecs1, decoded1, true);

        // 解码第二个分片
        std::vector<iovec> iovecs2(1);
        iovecs2[0].iov_base = const_cast<char*>(encoded2.data());
        iovecs2[0].iov_len = encoded2.size();
        WsFrame decoded2;
        WsFrameParser::fromIOVec(iovecs2, decoded2, true);
    }

    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start).count();

    std::cout << "  Iterations: " << iterations << " (x2 fragments)" << std::endl;
    std::cout << "  Time: " << duration << " ms" << std::endl;
    std::cout << "  Throughput: " << (iterations * 2 * 1000 / duration) << " ops/sec" << std::endl;
    std::cout << "  Avg time: " << (duration * 1000.0 / (iterations * 2)) << " μs/op" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "WebSocket Frame Parser Benchmark" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // 编码性能测试
    benchmark_frame_encoding_small();
    benchmark_frame_encoding_medium();
    benchmark_frame_encoding_large();

    // 解码性能测试
    benchmark_frame_decoding_small();
    benchmark_frame_decoding_medium();
    benchmark_frame_decoding_large();

    // 往返性能测试
    benchmark_frame_roundtrip();

    // 控制帧性能测试
    benchmark_control_frames();

    // 掩码性能测试
    benchmark_masking();

    // UTF-8 验证性能测试
    benchmark_utf8_validation();

    // 分片帧性能测试
    benchmark_fragmented_frames();

    std::cout << "\n========================================" << std::endl;
    std::cout << "Benchmark completed!" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
