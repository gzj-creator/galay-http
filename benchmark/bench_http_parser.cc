/**
 * @file test_http_parser_benchmark.cc
 * @brief HTTP Request/Response 解析性能压测
 *
 * 压测场景：
 * 1. 完整请求/响应解析性能
 * 2. 增量解析性能（模拟网络分片）
 * 3. RingBuffer环绕场景性能
 * 4. 大body解析性能
 * 5. 高并发场景模拟
 */

#include <iostream>
#include <chrono>
#include <vector>
#include <random>
#include <iomanip>
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/protoc/http/HttpError.h"
#include <common/Buffer.h>

using namespace galay::http;
using namespace galay::kernel;
using namespace std::chrono;

// 性能统计结构
struct BenchmarkStats {
    std::string name;
    size_t iterations;
    size_t total_bytes;
    double elapsed_ms;
    double ops_per_sec;
    double mb_per_sec;
    double avg_latency_us;
};

// 打印统计信息
void print_stats(const BenchmarkStats& stats) {
    std::cout << "\n[" << stats.name << "]" << std::endl;
    std::cout << "  Iterations:    " << stats.iterations << std::endl;
    std::cout << "  Total bytes:   " << stats.total_bytes << " bytes" << std::endl;
    std::cout << "  Elapsed time:  " << std::fixed << std::setprecision(2) << stats.elapsed_ms << " ms" << std::endl;
    std::cout << "  Throughput:    " << std::fixed << std::setprecision(2) << stats.ops_per_sec << " ops/sec" << std::endl;
    std::cout << "  Bandwidth:     " << std::fixed << std::setprecision(2) << stats.mb_per_sec << " MB/sec" << std::endl;
    std::cout << "  Avg latency:   " << std::fixed << std::setprecision(3) << stats.avg_latency_us << " μs" << std::endl;
}

// ============ 基准测试函数 ============

// 1. 完整请求解析性能测试
BenchmarkStats benchmark_complete_request_parsing(size_t iterations) {
    std::cout << "\n=== Benchmark: Complete Request Parsing ===" << std::endl;

    std::string req = "GET /api/users/12345?page=1&limit=10 HTTP/1.1\r\n"
                      "Host: example.com\r\n"
                      "User-Agent: Mozilla/5.0\r\n"
                      "Accept: application/json\r\n"
                      "Content-Length: 50\r\n"
                      "\r\n"
                      "12345678901234567890123456789012345678901234567890";

    size_t total_bytes = 0;
    auto start = high_resolution_clock::now();

    for (size_t i = 0; i < iterations; ++i) {
        RingBuffer buffer(4096);
        HttpRequest request;

        buffer.write(req);
        auto iovecs = buffer.getReadIovecs();
        auto [err, consumed] = request.fromIOVec(iovecs);

        if (err != kNoError || !request.isComplete()) {
            std::cerr << "Parse error at iteration " << i << std::endl;
            break;
        }

        total_bytes += req.size();
        buffer.consume(consumed);
    }

    auto end = high_resolution_clock::now();
    double elapsed_ms = duration_cast<microseconds>(end - start).count() / 1000.0;

    BenchmarkStats stats;
    stats.name = "Complete Request Parsing";
    stats.iterations = iterations;
    stats.total_bytes = total_bytes;
    stats.elapsed_ms = elapsed_ms;
    stats.ops_per_sec = (iterations / elapsed_ms) * 1000.0;
    stats.mb_per_sec = (total_bytes / (1024.0 * 1024.0)) / (elapsed_ms / 1000.0);
    stats.avg_latency_us = (elapsed_ms * 1000.0) / iterations;

    return stats;
}

// 2. 增量解析性能测试（模拟网络分片）
BenchmarkStats benchmark_incremental_parsing(size_t iterations) {
    std::cout << "\n=== Benchmark: Incremental Parsing ===" << std::endl;

    std::string req = "POST /api/data HTTP/1.1\r\n"
                      "Host: example.com\r\n"
                      "Content-Length: 100\r\n"
                      "\r\n"
                      + std::string(100, 'X');

    size_t total_bytes = 0;
    auto start = high_resolution_clock::now();

    for (size_t i = 0; i < iterations; ++i) {
        RingBuffer buffer(4096);
        HttpRequest request;

        // 模拟分片：每次写入10-30字节
        size_t offset = 0;
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(10, 30);

        while (offset < req.size()) {
            size_t chunk_size = std::min(static_cast<size_t>(dis(gen)), req.size() - offset);
            buffer.write(req.substr(offset, chunk_size));
            offset += chunk_size;

            auto iovecs = buffer.getReadIovecs();
            auto [err, consumed] = request.fromIOVec(iovecs);

            if (consumed > 0) {
                buffer.consume(consumed);
            }

            if (request.isComplete()) {
                break;
            }
        }

        total_bytes += req.size();
    }

    auto end = high_resolution_clock::now();
    double elapsed_ms = duration_cast<microseconds>(end - start).count() / 1000.0;

    BenchmarkStats stats;
    stats.name = "Incremental Parsing";
    stats.iterations = iterations;
    stats.total_bytes = total_bytes;
    stats.elapsed_ms = elapsed_ms;
    stats.ops_per_sec = (iterations / elapsed_ms) * 1000.0;
    stats.mb_per_sec = (total_bytes / (1024.0 * 1024.0)) / (elapsed_ms / 1000.0);
    stats.avg_latency_us = (elapsed_ms * 1000.0) / iterations;

    return stats;
}

// 3. RingBuffer环绕场景性能测试
BenchmarkStats benchmark_ringbuffer_wrap(size_t iterations) {
    std::cout << "\n=== Benchmark: RingBuffer Wrap Around ===" << std::endl;

    std::string req = "GET /wrap HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Content-Length: 20\r\n"
                      "\r\n"
                      "12345678901234567890";

    size_t total_bytes = 0;
    auto start = high_resolution_clock::now();

    for (size_t i = 0; i < iterations; ++i) {
        RingBuffer buffer(256);  // 小buffer强制环绕

        // 先填充buffer让写指针前进
        std::string filler(200, 'x');
        buffer.write(filler);
        buffer.consume(200);

        // 写入请求（会环绕）
        buffer.write(req);

        HttpRequest request;
        auto iovecs = buffer.getReadIovecs();
        auto [err, consumed] = request.fromIOVec(iovecs);

        if (err != kNoError || !request.isComplete()) {
            std::cerr << "Parse error at iteration " << i << std::endl;
            break;
        }

        total_bytes += req.size();
        buffer.consume(consumed);
    }

    auto end = high_resolution_clock::now();
    double elapsed_ms = duration_cast<microseconds>(end - start).count() / 1000.0;

    BenchmarkStats stats;
    stats.name = "RingBuffer Wrap Around";
    stats.iterations = iterations;
    stats.total_bytes = total_bytes;
    stats.elapsed_ms = elapsed_ms;
    stats.ops_per_sec = (iterations / elapsed_ms) * 1000.0;
    stats.mb_per_sec = (total_bytes / (1024.0 * 1024.0)) / (elapsed_ms / 1000.0);
    stats.avg_latency_us = (elapsed_ms * 1000.0) / iterations;

    return stats;
}

// 4. 大body解析性能测试
BenchmarkStats benchmark_large_body_parsing(size_t iterations, size_t body_size) {
    std::cout << "\n=== Benchmark: Large Body Parsing (" << body_size << " bytes) ===" << std::endl;

    std::string body(body_size, 'L');
    std::string req = "POST /upload HTTP/1.1\r\n"
                      "Host: example.com\r\n"
                      "Content-Length: " + std::to_string(body_size) + "\r\n"
                      "\r\n" + body;

    size_t total_bytes = 0;
    auto start = high_resolution_clock::now();

    for (size_t i = 0; i < iterations; ++i) {
        RingBuffer buffer(body_size + 4096);
        HttpRequest request;

        buffer.write(req);
        auto iovecs = buffer.getReadIovecs();
        auto [err, consumed] = request.fromIOVec(iovecs);

        if (err != kNoError || !request.isComplete()) {
            std::cerr << "Parse error at iteration " << i << std::endl;
            break;
        }

        total_bytes += req.size();
        buffer.consume(consumed);
    }

    auto end = high_resolution_clock::now();
    double elapsed_ms = duration_cast<microseconds>(end - start).count() / 1000.0;

    BenchmarkStats stats;
    stats.name = "Large Body Parsing (" + std::to_string(body_size) + " bytes)";
    stats.iterations = iterations;
    stats.total_bytes = total_bytes;
    stats.elapsed_ms = elapsed_ms;
    stats.ops_per_sec = (iterations / elapsed_ms) * 1000.0;
    stats.mb_per_sec = (total_bytes / (1024.0 * 1024.0)) / (elapsed_ms / 1000.0);
    stats.avg_latency_us = (elapsed_ms * 1000.0) / iterations;

    return stats;
}

// 5. 多请求连续解析性能测试
BenchmarkStats benchmark_multiple_requests(size_t iterations, size_t requests_per_batch) {
    std::cout << "\n=== Benchmark: Multiple Requests (" << requests_per_batch << " per batch) ===" << std::endl;

    std::string single_req = "GET /api/item HTTP/1.1\r\n"
                             "Host: example.com\r\n"
                             "\r\n";

    std::string batch;
    for (size_t i = 0; i < requests_per_batch; ++i) {
        batch += single_req;
    }

    size_t total_bytes = 0;
    size_t total_requests = 0;
    auto start = high_resolution_clock::now();

    for (size_t i = 0; i < iterations; ++i) {
        RingBuffer buffer(8192);
        buffer.write(batch);

        for (size_t j = 0; j < requests_per_batch; ++j) {
            HttpRequest request;
            auto iovecs = buffer.getReadIovecs();
            auto [err, consumed] = request.fromIOVec(iovecs);

            if (err != kNoError || !request.isComplete()) {
                std::cerr << "Parse error at iteration " << i << ", request " << j << std::endl;
                break;
            }

            buffer.consume(consumed);
            total_requests++;
        }

        total_bytes += batch.size();
    }

    auto end = high_resolution_clock::now();
    double elapsed_ms = duration_cast<microseconds>(end - start).count() / 1000.0;

    BenchmarkStats stats;
    stats.name = "Multiple Requests (" + std::to_string(requests_per_batch) + " per batch)";
    stats.iterations = total_requests;
    stats.total_bytes = total_bytes;
    stats.elapsed_ms = elapsed_ms;
    stats.ops_per_sec = (total_requests / elapsed_ms) * 1000.0;
    stats.mb_per_sec = (total_bytes / (1024.0 * 1024.0)) / (elapsed_ms / 1000.0);
    stats.avg_latency_us = (elapsed_ms * 1000.0) / total_requests;

    return stats;
}

// 6. Response解析性能测试
BenchmarkStats benchmark_response_parsing(size_t iterations) {
    std::cout << "\n=== Benchmark: Response Parsing ===" << std::endl;

    std::string resp = "HTTP/1.1 200 OK\r\n"
                       "Content-Type: application/json\r\n"
                       "Content-Length: 100\r\n"
                       "\r\n"
                       + std::string(100, 'R');

    size_t total_bytes = 0;
    auto start = high_resolution_clock::now();

    for (size_t i = 0; i < iterations; ++i) {
        RingBuffer buffer(4096);
        HttpResponse response;

        buffer.write(resp);
        auto iovecs = buffer.getReadIovecs();
        auto [err, consumed] = response.fromIOVec(iovecs);

        if (err != kNoError || !response.isComplete()) {
            std::cerr << "Parse error at iteration " << i << std::endl;
            break;
        }

        total_bytes += resp.size();
        buffer.consume(consumed);
    }

    auto end = high_resolution_clock::now();
    double elapsed_ms = duration_cast<microseconds>(end - start).count() / 1000.0;

    BenchmarkStats stats;
    stats.name = "Response Parsing";
    stats.iterations = iterations;
    stats.total_bytes = total_bytes;
    stats.elapsed_ms = elapsed_ms;
    stats.ops_per_sec = (iterations / elapsed_ms) * 1000.0;
    stats.mb_per_sec = (total_bytes / (1024.0 * 1024.0)) / (elapsed_ms / 1000.0);
    stats.avg_latency_us = (elapsed_ms * 1000.0) / iterations;

    return stats;
}

// 7. 极限压测：单字节增量解析
BenchmarkStats benchmark_single_byte_incremental(size_t iterations) {
    std::cout << "\n=== Benchmark: Single Byte Incremental (Stress Test) ===" << std::endl;

    std::string req = "GET /stress HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Content-Length: 10\r\n"
                      "\r\n"
                      "1234567890";

    size_t total_bytes = 0;
    auto start = high_resolution_clock::now();

    for (size_t i = 0; i < iterations; ++i) {
        RingBuffer buffer(4096);
        HttpRequest request;

        // 一次写入一个字节
        for (size_t j = 0; j < req.size(); ++j) {
            buffer.write(std::string(1, req[j]));

            auto iovecs = buffer.getReadIovecs();
            auto [err, consumed] = request.fromIOVec(iovecs);

            if (consumed > 0) {
                buffer.consume(consumed);
            }

            if (request.isComplete()) {
                break;
            }
        }

        total_bytes += req.size();
    }

    auto end = high_resolution_clock::now();
    double elapsed_ms = duration_cast<microseconds>(end - start).count() / 1000.0;

    BenchmarkStats stats;
    stats.name = "Single Byte Incremental (Stress)";
    stats.iterations = iterations;
    stats.total_bytes = total_bytes;
    stats.elapsed_ms = elapsed_ms;
    stats.ops_per_sec = (iterations / elapsed_ms) * 1000.0;
    stats.mb_per_sec = (total_bytes / (1024.0 * 1024.0)) / (elapsed_ms / 1000.0);
    stats.avg_latency_us = (elapsed_ms * 1000.0) / iterations;

    return stats;
}

// ============ 主函数 ============

int main(int argc, char* argv[]) {
    std::cout << "========================================" << std::endl;
    std::cout << "HTTP Parser Performance Benchmark" << std::endl;
    std::cout << "========================================" << std::endl;

    // 默认迭代次数
    size_t iterations = 100000;
    if (argc > 1) {
        iterations = std::stoull(argv[1]);
    }

    std::cout << "\nRunning benchmarks with " << iterations << " iterations..." << std::endl;

    std::vector<BenchmarkStats> all_stats;

    // 运行所有基准测试
    all_stats.push_back(benchmark_complete_request_parsing(iterations));
    all_stats.push_back(benchmark_incremental_parsing(iterations / 10));  // 增量解析较慢
    all_stats.push_back(benchmark_ringbuffer_wrap(iterations));
    all_stats.push_back(benchmark_large_body_parsing(iterations / 100, 1024));      // 1KB
    all_stats.push_back(benchmark_large_body_parsing(iterations / 100, 10240));     // 10KB
    all_stats.push_back(benchmark_large_body_parsing(iterations / 1000, 102400));   // 100KB
    all_stats.push_back(benchmark_multiple_requests(iterations / 10, 10));
    all_stats.push_back(benchmark_response_parsing(iterations));
    all_stats.push_back(benchmark_single_byte_incremental(iterations / 100));  // 极限压测

    // 打印所有统计信息
    std::cout << "\n========================================" << std::endl;
    std::cout << "Benchmark Results Summary" << std::endl;
    std::cout << "========================================" << std::endl;

    for (const auto& stats : all_stats) {
        print_stats(stats);
    }

    std::cout << "\n========================================" << std::endl;
    std::cout << "Benchmark completed successfully!" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
