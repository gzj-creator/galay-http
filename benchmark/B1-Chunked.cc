/**
 * @file bench_chunked.cc
 * @brief HTTP Chunked 编码性能测试
 */

#include <iostream>
#include <chrono>
#include <vector>
#include "galay-http/protoc/http/HttpChunk.h"
#include "galay-kernel/common/Buffer.h"

using namespace galay::http;
using namespace galay::kernel;
using namespace std::chrono;

void benchmark_chunk_encoding() {
    std::cout << "=== Chunk Encoding Benchmark ===" << std::endl;

    const int iterations = 100000;
    std::string testData = "This is a test chunk data for benchmarking performance";

    auto start = high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        auto encoded = Chunk::toChunk(testData, false);
    }

    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start).count();

    std::cout << "  Iterations: " << iterations << std::endl;
    std::cout << "  Time: " << duration << " ms" << std::endl;
    std::cout << "  Throughput: " << (iterations * 1000 / duration) << " ops/sec" << std::endl;
    std::cout << "  Avg time: " << (duration * 1000.0 / iterations) << " μs/op" << std::endl;
}

void benchmark_chunk_decoding() {
    std::cout << "\n=== Chunk Decoding Benchmark ===" << std::endl;

    const int iterations = 100000;

    // 准备测试数据
    std::string data1 = "Hello ";
    std::string data2 = "World!";
    std::string data3 = "Test";
    std::string chunk1 = Chunk::toChunk(data1, false);
    std::string chunk2 = Chunk::toChunk(data2, false);
    std::string chunk3 = Chunk::toChunk(data3, false);
    std::string empty;
    std::string lastChunk = Chunk::toChunk(empty, true);
    std::string allChunks = chunk1 + chunk2 + chunk3 + lastChunk;

    std::vector<iovec> iovecs(1);
    iovecs[0].iov_base = const_cast<char*>(allChunks.data());
    iovecs[0].iov_len = allChunks.size();

    auto start = high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        std::string output;
        auto result = Chunk::fromIOVec(iovecs, output);
    }

    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start).count();

    std::cout << "  Iterations: " << iterations << std::endl;
    std::cout << "  Time: " << duration << " ms" << std::endl;
    std::cout << "  Throughput: " << (iterations * 1000 / duration) << " ops/sec" << std::endl;
    std::cout << "  Avg time: " << (duration * 1000.0 / iterations) << " μs/op" << std::endl;
}

void benchmark_with_ringbuffer() {
    std::cout << "\n=== Chunk with RingBuffer Benchmark ===" << std::endl;

    const int iterations = 50000;

    // 准备测试数据
    std::string data1 = "Hello World!";
    std::string data2 = "Test Data";
    std::string chunk1 = Chunk::toChunk(data1, false);
    std::string chunk2 = Chunk::toChunk(data2, false);
    std::string empty;
    std::string lastChunk = Chunk::toChunk(empty, true);
    std::string allChunks = chunk1 + chunk2 + lastChunk;

    auto start = high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        RingBuffer ringBuffer(8192);

        // 写入数据
        auto writeIovecs = ringBuffer.getWriteIovecs();
        size_t written = 0;
        for (const auto& iov : writeIovecs) {
            size_t toCopy = std::min(iov.iov_len, allChunks.size() - written);
            if (toCopy == 0) break;
            memcpy(iov.iov_base, allChunks.data() + written, toCopy);
            written += toCopy;
        }
        ringBuffer.produce(written);

        // 解析chunk
        std::string output;
        bool isLast = false;
        while (!isLast) {
            auto readIovecs = ringBuffer.getReadIovecs();
            if (readIovecs.empty()) break;

            auto result = Chunk::fromIOVec(readIovecs, output);
            if (!result) break;

            auto [last, consumed] = result.value();
            isLast = last;
            ringBuffer.consume(consumed);
        }
    }

    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start).count();

    std::cout << "  Iterations: " << iterations << std::endl;
    std::cout << "  Time: " << duration << " ms" << std::endl;
    std::cout << "  Throughput: " << (iterations * 1000 / duration) << " ops/sec" << std::endl;
    std::cout << "  Avg time: " << (duration * 1000.0 / iterations) << " μs/op" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "HTTP Chunked Encoding Benchmark" << std::endl;
    std::cout << "========================================\n" << std::endl;

    benchmark_chunk_encoding();
    benchmark_chunk_decoding();
    benchmark_with_ringbuffer();

    std::cout << "\n========================================" << std::endl;
    std::cout << "Benchmark completed!" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
