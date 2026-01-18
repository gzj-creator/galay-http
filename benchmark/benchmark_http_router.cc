/**
 * @file benchmark_http_router.cc
 * @brief HttpRouter 性能测试和压力测试
 */

#include <iostream>
#include <chrono>
#include <random>
#include <vector>
#include <string>
#include <iomanip>
#include "galay-http/kernel/http/HttpRouter.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-kernel/common/Log.h"

using namespace galay::http;
using namespace galay::kernel;
using namespace std::chrono;

// 测试用的处理器
Coroutine dummyHandler(HttpConn& conn, HttpRequest req) {
    co_return;
}

// 性能计时器
class BenchTimer {
public:
    BenchTimer() : start_(high_resolution_clock::now()) {}

    double elapsed() const {
        auto end = high_resolution_clock::now();
        return duration_cast<microseconds>(end - start_).count() / 1000.0;
    }

    void reset() {
        start_ = high_resolution_clock::now();
    }

private:
    high_resolution_clock::time_point start_;
};

// 生成随机字符串
std::string randomString(size_t length) {
    static const char charset[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, sizeof(charset) - 2);

    std::string result;
    result.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        result += charset[dis(gen)];
    }
    return result;
}

// 生成随机数字
int randomInt(int min, int max) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(min, max);
    return dis(gen);
}

void printStats(const std::string& name, double totalTime, size_t operations) {
    double avgTime = totalTime / operations;
    double opsPerSec = (operations * 1000.0) / totalTime;

    std::cout << std::left << std::setw(40) << name
              << std::right << std::setw(12) << std::fixed << std::setprecision(3) << totalTime << " ms"
              << std::setw(15) << std::fixed << std::setprecision(6) << avgTime << " ms"
              << std::setw(18) << std::fixed << std::setprecision(0) << opsPerSec << " ops/s"
              << std::endl;
}

void benchmarkExactMatch() {
    LogInfo("========================================");
    LogInfo("Benchmark 1: Exact Match Performance");
    LogInfo("========================================");

    HttpRouter router;
    const size_t numRoutes = 1000;
    const size_t numLookups = 100000;

    // 添加路由
    std::vector<std::string> paths;
    BenchTimer timer;

    for (size_t i = 0; i < numRoutes; ++i) {
        std::string path = "/api/endpoint" + std::to_string(i);
        paths.push_back(path);
        router.addHandler<HttpMethod::GET>(path, dummyHandler);
    }

    double addTime = timer.elapsed();
    LogInfo("Added {} routes in {:.3f} ms", numRoutes, addTime);

    // 查找测试
    timer.reset();
    size_t found = 0;

    for (size_t i = 0; i < numLookups; ++i) {
        size_t idx = i % numRoutes;
        auto match = router.findHandler(HttpMethod::GET, paths[idx]);
        if (match.handler) found++;
    }

    double lookupTime = timer.elapsed();

    std::cout << "\nResults:" << std::endl;
    std::cout << std::string(85, '-') << std::endl;
    std::cout << std::left << std::setw(40) << "Operation"
              << std::right << std::setw(12) << "Total Time"
              << std::setw(15) << "Avg Time"
              << std::setw(18) << "Throughput"
              << std::endl;
    std::cout << std::string(85, '-') << std::endl;

    printStats("Add " + std::to_string(numRoutes) + " routes", addTime, numRoutes);
    printStats("Lookup " + std::to_string(numLookups) + " times", lookupTime, numLookups);

    std::cout << std::string(85, '-') << std::endl;
    LogInfo("Found: {}/{} ({:.1f}%)\n", found, numLookups, (found * 100.0) / numLookups);
}

void benchmarkPathParameters() {
    LogInfo("========================================");
    LogInfo("Benchmark 2: Path Parameter Performance");
    LogInfo("========================================");

    HttpRouter router;
    const size_t numRoutes = 100;
    const size_t numLookups = 50000;

    // 添加参数路由
    std::vector<std::string> patterns;
    BenchTimer timer;

    for (size_t i = 0; i < numRoutes; ++i) {
        std::string pattern = "/api/resource" + std::to_string(i) + "/:id";
        patterns.push_back(pattern);
        router.addHandler<HttpMethod::GET>(pattern, dummyHandler);
    }

    double addTime = timer.elapsed();
    LogInfo("Added {} param routes in {:.3f} ms", numRoutes, addTime);

    // 查找测试
    timer.reset();
    size_t found = 0;
    size_t totalParams = 0;

    for (size_t i = 0; i < numLookups; ++i) {
        size_t idx = i % numRoutes;
        std::string path = "/api/resource" + std::to_string(idx) + "/" + std::to_string(randomInt(1, 10000));
        auto match = router.findHandler(HttpMethod::GET, path);
        if (match.handler) {
            found++;
            totalParams += match.params.size();
        }
    }

    double lookupTime = timer.elapsed();

    std::cout << "\nResults:" << std::endl;
    std::cout << std::string(85, '-') << std::endl;
    std::cout << std::left << std::setw(40) << "Operation"
              << std::right << std::setw(12) << "Total Time"
              << std::setw(15) << "Avg Time"
              << std::setw(18) << "Throughput"
              << std::endl;
    std::cout << std::string(85, '-') << std::endl;

    printStats("Add " + std::to_string(numRoutes) + " param routes", addTime, numRoutes);
    printStats("Lookup " + std::to_string(numLookups) + " times", lookupTime, numLookups);

    std::cout << std::string(85, '-') << std::endl;
    LogInfo("Found: {}/{} ({:.1f}%)", found, numLookups, (found * 100.0) / numLookups);
    LogInfo("Avg params extracted: {:.2f}\n", (double)totalParams / found);
}

void benchmarkMixedRoutes() {
    LogInfo("========================================");
    LogInfo("Benchmark 3: Mixed Routes (Exact + Param + Wildcard)");
    LogInfo("========================================");

    HttpRouter router;
    const size_t exactRoutes = 500;
    const size_t paramRoutes = 300;
    const size_t wildcardRoutes = 200;
    const size_t numLookups = 100000;

    std::vector<std::string> exactPaths;
    std::vector<std::string> paramPatterns;
    std::vector<std::string> wildcardPatterns;

    BenchTimer timer;

    // 添加精确路由
    for (size_t i = 0; i < exactRoutes; ++i) {
        std::string path = "/exact/" + randomString(8);
        exactPaths.push_back(path);
        router.addHandler<HttpMethod::GET>(path, dummyHandler);
    }

    // 添加参数路由
    for (size_t i = 0; i < paramRoutes; ++i) {
        std::string pattern = "/param/" + randomString(6) + "/:id";
        paramPatterns.push_back(pattern);
        router.addHandler<HttpMethod::GET>(pattern, dummyHandler);
    }

    // 添加通配符路由
    for (size_t i = 0; i < wildcardRoutes; ++i) {
        std::string pattern = "/wildcard/" + randomString(5) + "/*";
        wildcardPatterns.push_back(pattern);
        router.addHandler<HttpMethod::GET>(pattern, dummyHandler);
    }

    double addTime = timer.elapsed();
    LogInfo("Added {} total routes in {:.3f} ms", router.size(), addTime);
    LogInfo("  - {} exact routes", exactRoutes);
    LogInfo("  - {} param routes", paramRoutes);
    LogInfo("  - {} wildcard routes", wildcardRoutes);

    // 混合查找测试
    timer.reset();
    size_t foundExact = 0, foundParam = 0, foundWildcard = 0, notFound = 0;

    for (size_t i = 0; i < numLookups; ++i) {
        int type = randomInt(0, 3);
        std::string path;

        if (type == 0 && !exactPaths.empty()) {
            // 查找精确路由
            path = exactPaths[randomInt(0, exactPaths.size() - 1)];
            auto match = router.findHandler(HttpMethod::GET, path);
            if (match.handler) foundExact++;
        } else if (type == 1 && !paramPatterns.empty()) {
            // 查找参数路由
            std::string pattern = paramPatterns[randomInt(0, paramPatterns.size() - 1)];
            size_t pos = pattern.find("/:id");
            path = pattern.substr(0, pos) + "/" + std::to_string(randomInt(1, 1000));
            auto match = router.findHandler(HttpMethod::GET, path);
            if (match.handler) foundParam++;
        } else if (type == 2 && !wildcardPatterns.empty()) {
            // 查找通配符路由
            std::string pattern = wildcardPatterns[randomInt(0, wildcardPatterns.size() - 1)];
            size_t pos = pattern.find("/*");
            path = pattern.substr(0, pos) + "/" + randomString(5);
            auto match = router.findHandler(HttpMethod::GET, path);
            if (match.handler) foundWildcard++;
        } else {
            // 查找不存在的路由
            path = "/notfound/" + randomString(10);
            auto match = router.findHandler(HttpMethod::GET, path);
            if (!match.handler) notFound++;
        }
    }

    double lookupTime = timer.elapsed();

    std::cout << "\nResults:" << std::endl;
    std::cout << std::string(85, '-') << std::endl;
    std::cout << std::left << std::setw(40) << "Operation"
              << std::right << std::setw(12) << "Total Time"
              << std::setw(15) << "Avg Time"
              << std::setw(18) << "Throughput"
              << std::endl;
    std::cout << std::string(85, '-') << std::endl;

    printStats("Add " + std::to_string(router.size()) + " mixed routes", addTime, router.size());
    printStats("Lookup " + std::to_string(numLookups) + " times", lookupTime, numLookups);

    std::cout << std::string(85, '-') << std::endl;
    LogInfo("Match statistics:");
    LogInfo("  - Exact matches: {}", foundExact);
    LogInfo("  - Param matches: {}", foundParam);
    LogInfo("  - Wildcard matches: {}", foundWildcard);
    LogInfo("  - Not found: {}\n", notFound);
}

void stressTestConcurrentLookup() {
    LogInfo("========================================");
    LogInfo("Stress Test: High-Frequency Lookups");
    LogInfo("========================================");

    HttpRouter router;
    const size_t numRoutes = 1000;
    const size_t numLookups = 1000000;  // 1 million lookups

    // 准备路由
    std::vector<std::string> paths;
    for (size_t i = 0; i < numRoutes; ++i) {
        std::string path = "/stress/endpoint" + std::to_string(i);
        paths.push_back(path);
        router.addHandler<HttpMethod::GET>(path, dummyHandler);
    }

    LogInfo("Prepared {} routes", numRoutes);
    LogInfo("Starting {} lookups...", numLookups);

    BenchTimer timer;
    size_t found = 0;

    for (size_t i = 0; i < numLookups; ++i) {
        size_t idx = randomInt(0, numRoutes - 1);
        auto match = router.findHandler(HttpMethod::GET, paths[idx]);
        if (match.handler) found++;
    }

    double totalTime = timer.elapsed();

    std::cout << "\nStress Test Results:" << std::endl;
    std::cout << std::string(85, '-') << std::endl;
    std::cout << std::left << std::setw(40) << "Operation"
              << std::right << std::setw(12) << "Total Time"
              << std::setw(15) << "Avg Time"
              << std::setw(18) << "Throughput"
              << std::endl;
    std::cout << std::string(85, '-') << std::endl;

    printStats(std::to_string(numLookups) + " random lookups", totalTime, numLookups);

    std::cout << std::string(85, '-') << std::endl;
    LogInfo("Success rate: {:.2f}%", (found * 100.0) / numLookups);
    LogInfo("Memory usage: ~{} KB (estimated)\n", (router.size() * 100) / 1024);
}

void benchmarkScalability() {
    LogInfo("========================================");
    LogInfo("Benchmark 4: Scalability Test");
    LogInfo("========================================");

    std::vector<size_t> routeCounts = {100, 500, 1000, 5000, 10000};
    const size_t lookupsPerTest = 10000;

    std::cout << "\nScalability Results:" << std::endl;
    std::cout << std::string(85, '-') << std::endl;
    std::cout << std::left << std::setw(15) << "Routes"
              << std::right << std::setw(15) << "Add Time (ms)"
              << std::setw(20) << "Lookup Time (ms)"
              << std::setw(20) << "Avg Lookup (μs)"
              << std::setw(15) << "Throughput"
              << std::endl;
    std::cout << std::string(85, '-') << std::endl;

    for (size_t numRoutes : routeCounts) {
        HttpRouter router;
        std::vector<std::string> paths;

        // 添加路由
        BenchTimer addTimer;
        for (size_t i = 0; i < numRoutes; ++i) {
            std::string path = "/scale/endpoint" + std::to_string(i);
            paths.push_back(path);
            router.addHandler<HttpMethod::GET>(path, dummyHandler);
        }
        double addTime = addTimer.elapsed();

        // 查找测试
        BenchTimer lookupTimer;
        for (size_t i = 0; i < lookupsPerTest; ++i) {
            size_t idx = randomInt(0, numRoutes - 1);
            router.findHandler(HttpMethod::GET, paths[idx]);
        }
        double lookupTime = lookupTimer.elapsed();
        double avgLookup = (lookupTime * 1000.0) / lookupsPerTest;
        double throughput = (lookupsPerTest * 1000.0) / lookupTime;

        std::cout << std::left << std::setw(15) << numRoutes
                  << std::right << std::setw(15) << std::fixed << std::setprecision(3) << addTime
                  << std::setw(20) << std::fixed << std::setprecision(3) << lookupTime
                  << std::setw(20) << std::fixed << std::setprecision(3) << avgLookup
                  << std::setw(15) << std::fixed << std::setprecision(0) << throughput
                  << std::endl;
    }

    std::cout << std::string(85, '-') << std::endl;
    LogInfo("");
}

int main() {
    LogInfo("========================================");
    LogInfo("HttpRouter Performance Benchmark");
    LogInfo("========================================\n");

    try {
        benchmarkExactMatch();
        benchmarkPathParameters();
        benchmarkMixedRoutes();
        benchmarkScalability();
        stressTestConcurrentLookup();

        LogInfo("========================================");
        LogInfo("✓ ALL BENCHMARKS COMPLETED!");
        LogInfo("========================================");
        return 0;
    } catch (const std::exception& e) {
        LogError("Benchmark failed with exception: {}", e.what());
        return 1;
    }
}
