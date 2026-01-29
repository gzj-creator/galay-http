/**
 * @file bench_file_transfer.cc
 * @brief 文件传输性能基准测试
 *
 * 测试场景：
 * 1. MEMORY 模式性能测试（小文件）
 * 2. CHUNK 模式性能测试（中等文件）
 * 3. SENDFILE 模式性能测试（大文件）
 * 4. AUTO 模式性能测试
 * 5. 不同文件大小的性能对比
 */

#include <iostream>
#include <chrono>
#include <vector>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <thread>
#include "galay-http/kernel/http/HttpServer.h"
#include "galay-http/kernel/http/HttpRouter.h"
#include "galay-http/kernel/http/HttpClient.h"
#include "galay-http/kernel/http/StaticFileConfig.h"
#include "galay-kernel/kernel/Runtime.h"

using namespace galay::http;
using namespace galay::kernel;
using namespace std::chrono;
namespace fs = std::filesystem;

// 性能统计结构
struct BenchmarkStats {
    std::string name;
    std::string mode;
    size_t file_size;
    size_t iterations;
    double elapsed_ms;
    double ops_per_sec;
    double mb_per_sec;
    double avg_latency_ms;
};

// 打印统计信息
void print_stats(const BenchmarkStats& stats) {
    std::cout << "\n[" << stats.name << "]" << std::endl;
    std::cout << "  Mode:          " << stats.mode << std::endl;
    std::cout << "  File size:     " << stats.file_size << " bytes ("
              << std::fixed << std::setprecision(2) << (stats.file_size / 1024.0) << " KB)" << std::endl;
    std::cout << "  Iterations:    " << stats.iterations << std::endl;
    std::cout << "  Elapsed time:  " << std::fixed << std::setprecision(2) << stats.elapsed_ms << " ms" << std::endl;
    std::cout << "  Throughput:    " << std::fixed << std::setprecision(2) << stats.ops_per_sec << " ops/sec" << std::endl;
    std::cout << "  Bandwidth:     " << std::fixed << std::setprecision(2) << stats.mb_per_sec << " MB/sec" << std::endl;
    std::cout << "  Avg latency:   " << std::fixed << std::setprecision(3) << stats.avg_latency_ms << " ms" << std::endl;
}

// 创建测试文件
void create_test_file(const std::string& path, size_t size) {
    std::ofstream file(path, std::ios::binary);

    // 生成随机数据
    std::vector<char> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<char>(i % 256);
    }

    file.write(data.data(), size);
    file.close();
}

// 客户端下载协程
Coroutine download_file(Runtime& runtime, const std::string& url, size_t& bytes_received) {
    HttpClient client;

    auto connect_result = co_await client.connect(url);
    if (!connect_result) {
        std::cerr << "Connection failed\n";
        co_return;
    }

    while (true) {
        auto result = co_await client.get(client.url().path);

        if (!result) {
            std::cerr << "Request failed\n";
            co_return;
        }

        if (!result.value()) {
            continue;
        }

        auto response = result.value().value();
        bytes_received = response.getBodyStr().size();
        break;
    }

    co_await client.close();
    co_return;
}

// 基准测试函数
BenchmarkStats benchmark_file_transfer(
    const std::string& test_name,
    const std::string& mode_name,
    FileTransferMode mode,
    size_t file_size,
    size_t iterations,
    int port
) {
    std::cout << "\n=== Benchmark: " << test_name << " ===" << std::endl;

    // 创建测试目录和文件
    fs::create_directories("./bench_files");
    std::string file_path = "./bench_files/test_" + std::to_string(file_size) + ".bin";
    create_test_file(file_path, file_size);

    // 配置服务器
    HttpRouter router;
    StaticFileConfig config;
    config.setTransferMode(mode);
    router.mount("/files", "./bench_files", config);

    HttpServerConfig server_config;
    server_config.host = "127.0.0.1";
    server_config.port = port;
    server_config.io_scheduler_count = 2;

    HttpServer server(server_config);

    // 启动服务器（在后台线程）
    std::thread server_thread([&]() {
        server.start(std::move(router));
    });

    // 等待服务器启动
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 创建客户端 Runtime
    Runtime client_runtime(LoadBalanceStrategy::ROUND_ROBIN, 2, 1);
    client_runtime.start();

    // 执行基准测试
    size_t total_bytes = 0;
    auto start = high_resolution_clock::now();

    for (size_t i = 0; i < iterations; ++i) {
        size_t bytes_received = 0;

        auto* scheduler = client_runtime.getNextIOScheduler();
        scheduler->spawn(download_file(
            client_runtime,
            "http://127.0.0.1:" + std::to_string(port) + "/files/test_" + std::to_string(file_size) + ".bin",
            bytes_received
        ));

        // 等待下载完成
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        total_bytes += bytes_received;
    }

    auto end = high_resolution_clock::now();
    double elapsed_ms = duration_cast<milliseconds>(end - start).count();

    // 停止服务器和客户端
    client_runtime.stop();
    server.stop();
    server_thread.join();

    // 清理测试文件
    fs::remove(file_path);

    // 计算统计信息
    BenchmarkStats stats;
    stats.name = test_name;
    stats.mode = mode_name;
    stats.file_size = file_size;
    stats.iterations = iterations;
    stats.elapsed_ms = elapsed_ms;
    stats.ops_per_sec = (iterations / elapsed_ms) * 1000.0;
    stats.mb_per_sec = (total_bytes / (1024.0 * 1024.0)) / (elapsed_ms / 1000.0);
    stats.avg_latency_ms = elapsed_ms / iterations;

    return stats;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "File Transfer Performance Benchmark" << std::endl;
    std::cout << "========================================" << std::endl;

    std::vector<BenchmarkStats> all_stats;
    int base_port = 9000;

    // 1. MEMORY 模式 - 小文件（16KB）
    all_stats.push_back(benchmark_file_transfer(
        "MEMORY Mode - Small File",
        "MEMORY",
        FileTransferMode::MEMORY,
        16 * 1024,      // 16KB
        100,            // 100 次迭代
        base_port++
    ));

    // 2. MEMORY 模式 - 中等文件（64KB）
    all_stats.push_back(benchmark_file_transfer(
        "MEMORY Mode - Medium File",
        "MEMORY",
        FileTransferMode::MEMORY,
        64 * 1024,      // 64KB
        50,
        base_port++
    ));

    // 3. CHUNK 模式 - 中等文件（128KB）
    all_stats.push_back(benchmark_file_transfer(
        "CHUNK Mode - Medium File",
        "CHUNK",
        FileTransferMode::CHUNK,
        128 * 1024,     // 128KB
        50,
        base_port++
    ));

    // 4. CHUNK 模式 - 大文件（512KB）
    all_stats.push_back(benchmark_file_transfer(
        "CHUNK Mode - Large File",
        "CHUNK",
        FileTransferMode::CHUNK,
        512 * 1024,     // 512KB
        20,
        base_port++
    ));

    // 5. SENDFILE 模式 - 大文件（2MB）
    all_stats.push_back(benchmark_file_transfer(
        "SENDFILE Mode - Large File",
        "SENDFILE",
        FileTransferMode::SENDFILE,
        2 * 1024 * 1024,  // 2MB
        20,
        base_port++
    ));

    // 6. SENDFILE 模式 - 超大文件（10MB）
    all_stats.push_back(benchmark_file_transfer(
        "SENDFILE Mode - Very Large File",
        "SENDFILE",
        FileTransferMode::SENDFILE,
        10 * 1024 * 1024,  // 10MB
        10,
        base_port++
    ));

    // 7. AUTO 模式 - 小文件（32KB）
    all_stats.push_back(benchmark_file_transfer(
        "AUTO Mode - Small File",
        "AUTO",
        FileTransferMode::AUTO,
        32 * 1024,      // 32KB
        50,
        base_port++
    ));

    // 8. AUTO 模式 - 中等文件（256KB）
    all_stats.push_back(benchmark_file_transfer(
        "AUTO Mode - Medium File",
        "AUTO",
        FileTransferMode::AUTO,
        256 * 1024,     // 256KB
        30,
        base_port++
    ));

    // 9. AUTO 模式 - 大文件（5MB）
    all_stats.push_back(benchmark_file_transfer(
        "AUTO Mode - Large File",
        "AUTO",
        FileTransferMode::AUTO,
        5 * 1024 * 1024,  // 5MB
        10,
        base_port++
    ));

    // 打印所有统计信息
    std::cout << "\n\n========================================" << std::endl;
    std::cout << "Benchmark Results Summary" << std::endl;
    std::cout << "========================================" << std::endl;

    for (const auto& stats : all_stats) {
        print_stats(stats);
    }

    // 打印对比表格
    std::cout << "\n\n========================================" << std::endl;
    std::cout << "Performance Comparison Table" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::left << std::setw(30) << "Test Name"
              << std::setw(12) << "Mode"
              << std::setw(15) << "File Size"
              << std::setw(15) << "Throughput"
              << std::setw(15) << "Bandwidth"
              << std::setw(15) << "Avg Latency" << std::endl;
    std::cout << std::string(102, '-') << std::endl;

    for (const auto& stats : all_stats) {
        std::cout << std::left << std::setw(30) << stats.name
                  << std::setw(12) << stats.mode
                  << std::setw(15) << (std::to_string(stats.file_size / 1024) + " KB")
                  << std::setw(15) << (std::to_string(static_cast<int>(stats.ops_per_sec)) + " ops/s")
                  << std::setw(15) << (std::to_string(static_cast<int>(stats.mb_per_sec)) + " MB/s")
                  << std::setw(15) << (std::to_string(static_cast<int>(stats.avg_latency_ms)) + " ms")
                  << std::endl;
    }

    // 清理测试目录
    fs::remove_all("./bench_files");

    std::cout << "\n========================================" << std::endl;
    std::cout << "Benchmark completed!" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
