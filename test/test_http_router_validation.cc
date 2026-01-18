/**
 * @file test_http_router_validation.cc
 * @brief HttpRouter 路径验证测试
 */

#include <iostream>
#include <cassert>
#include "galay-http/kernel/http/HttpRouter.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-kernel/common/Log.h"

using namespace galay::http;
using namespace galay::kernel;

// 测试用的处理器
Coroutine testHandler(HttpConn& conn, HttpRequest req) {
    co_return;
}

void testValidPaths() {
    LogInfo("========================================");
    LogInfo("Test 1: Valid Paths");
    LogInfo("========================================");

    HttpRouter router;

    // 有效的路径
    std::vector<std::string> validPaths = {
        "/",
        "/api",
        "/api/users",
        "/api/users/:id",
        "/api/users/:userId/posts/:postId",
        "/user/:id",
        "/static/*",
        "/files/**",
        "/api/v1/users",
        "/path-with-dash",
        "/path_with_underscore",
        "/path.with.dot",
        "/path~with~tilde",
        "/api/users/:user_id",
        "/api/users/:userId123"
    };

    size_t successCount = 0;
    for (const auto& path : validPaths) {
        size_t beforeSize = router.size();
        router.addHandler<HttpMethod::GET>(path, testHandler);
        size_t afterSize = router.size();

        if (afterSize > beforeSize) {
            LogInfo("✓ Valid path accepted: {}", path);
            successCount++;
        } else {
            LogError("✗ Valid path rejected: {}", path);
        }
    }

    LogInfo("Valid paths: {}/{} accepted\n", successCount, validPaths.size());
    assert(successCount == validPaths.size());
}

void testInvalidPaths() {
    LogInfo("========================================");
    LogInfo("Test 2: Invalid Paths (Should be Rejected)");
    LogInfo("========================================");

    HttpRouter router;

    // 无效的路径
    struct TestCase {
        std::string path;
        std::string reason;
    };

    std::vector<TestCase> invalidPaths = {
        {"", "Empty path"},
        {"api/users", "Missing leading /"},
        {"/api/users/:id/:id", "Duplicate parameter name"},
        {"/api/*/extra", "Wildcard not at end"},
        {"/api/**/extra", "Greedy wildcard not at end"},
        {"/api/:", "Empty parameter name"},
        {"/api/:user-id", "Invalid character in parameter name"},
        {"/api/:user id", "Space in parameter name"},
        {"/api/users/*/posts", "Wildcard not at end"},
        {"/api/users/**/**", "Multiple wildcards"},
        {"/api/users/:id/:name/:id", "Duplicate parameter name (3 params)"},
        {"/api/users/:123", "Parameter name starts with number"},
        {"/api/users/:user@id", "Invalid character @ in parameter"},
        {"/api/users/:user#id", "Invalid character # in parameter"}
    };

    size_t rejectedCount = 0;
    for (const auto& [path, reason] : invalidPaths) {
        size_t beforeSize = router.size();
        router.addHandler<HttpMethod::GET>(path, testHandler);
        size_t afterSize = router.size();

        if (afterSize == beforeSize) {
            LogInfo("✓ Invalid path rejected: {} ({})", path, reason);
            rejectedCount++;
        } else {
            LogError("✗ Invalid path accepted: {} ({})", path, reason);
        }
    }

    LogInfo("Invalid paths: {}/{} rejected\n", rejectedCount, invalidPaths.size());
    assert(rejectedCount == invalidPaths.size());
}

void testDuplicateRoutes() {
    LogInfo("========================================");
    LogInfo("Test 3: Duplicate Route Detection");
    LogInfo("========================================");

    HttpRouter router;

    // 第一次添加
    router.addHandler<HttpMethod::GET>("/api/users", testHandler);
    size_t size1 = router.size();
    LogInfo("Added route /api/users, size: {}", size1);

    // 第二次添加相同路由（应该覆盖并警告）
    router.addHandler<HttpMethod::GET>("/api/users", testHandler);
    size_t size2 = router.size();
    LogInfo("Added duplicate route /api/users, size: {}", size2);

    // 大小应该相同（覆盖而不是新增）
    assert(size1 == size2);
    LogInfo("✓ Duplicate route detected and handled correctly\n");
}

void testParameterExtraction() {
    LogInfo("========================================");
    LogInfo("Test 4: Parameter Extraction with Validation");
    LogInfo("========================================");

    HttpRouter router;

    // 注册带参数的路由
    router.addHandler<HttpMethod::GET>("/user/:id", testHandler);
    router.addHandler<HttpMethod::GET>("/user/:userId/posts/:postId", testHandler);

    // 测试参数提取
    auto match1 = router.findHandler(HttpMethod::GET, "/user/123");
    assert(match1.handler != nullptr);
    assert(match1.params.size() == 1);
    assert(match1.params["id"] == "123");
    LogInfo("✓ Single parameter extracted: id={}", match1.params["id"]);

    auto match2 = router.findHandler(HttpMethod::GET, "/user/456/posts/789");
    assert(match2.handler != nullptr);
    assert(match2.params.size() == 2);
    assert(match2.params["userId"] == "456");
    assert(match2.params["postId"] == "789");
    LogInfo("✓ Multiple parameters extracted: userId={}, postId={}",
            match2.params["userId"], match2.params["postId"]);

    LogInfo("✓ All parameter extraction tests passed\n");
}

void testEdgeCases() {
    LogInfo("========================================");
    LogInfo("Test 5: Edge Cases");
    LogInfo("========================================");

    HttpRouter router;

    // 根路径
    router.addHandler<HttpMethod::GET>("/", testHandler);
    auto match1 = router.findHandler(HttpMethod::GET, "/");
    assert(match1.handler != nullptr);
    LogInfo("✓ Root path / works");

    // 很长的路径（但在限制内）
    std::string longPath = "/api";
    for (int i = 0; i < 50; ++i) {
        longPath += "/segment" + std::to_string(i);
    }
    router.addHandler<HttpMethod::GET>(longPath, testHandler);
    auto match2 = router.findHandler(HttpMethod::GET, longPath);
    assert(match2.handler != nullptr);
    LogInfo("✓ Long path works (length: {})", longPath.length());

    // 多个参数
    router.addHandler<HttpMethod::GET>("/a/:p1/b/:p2/c/:p3/d/:p4", testHandler);
    auto match3 = router.findHandler(HttpMethod::GET, "/a/1/b/2/c/3/d/4");
    assert(match3.handler != nullptr);
    assert(match3.params.size() == 4);
    LogInfo("✓ Multiple parameters work: {} params extracted", match3.params.size());

    LogInfo("✓ All edge case tests passed\n");
}

void testHttpRequestIntegration() {
    LogInfo("========================================");
    LogInfo("Test 6: HttpRequest Integration");
    LogInfo("========================================");

    HttpRouter router;
    router.addHandler<HttpMethod::GET>("/user/:id/posts/:postId", testHandler);

    // 模拟路由匹配和参数设置
    auto match = router.findHandler(HttpMethod::GET, "/user/123/posts/456");
    assert(match.handler != nullptr);

    // 创建 HttpRequest 并设置参数
    HttpRequest request;
    request.setRouteParams(std::move(match.params));

    // 验证参数可以从 HttpRequest 中获取
    assert(request.hasRouteParam("id"));
    assert(request.hasRouteParam("postId"));
    assert(request.getRouteParam("id") == "123");
    assert(request.getRouteParam("postId") == "456");
    assert(request.getRouteParam("nonexistent", "default") == "default");

    LogInfo("✓ HttpRequest.getRouteParam('id') = {}", request.getRouteParam("id"));
    LogInfo("✓ HttpRequest.getRouteParam('postId') = {}", request.getRouteParam("postId"));
    LogInfo("✓ HttpRequest.hasRouteParam('id') = {}", request.hasRouteParam("id"));
    LogInfo("✓ HttpRequest.getRouteParam('nonexistent', 'default') = {}",
            request.getRouteParam("nonexistent", "default"));

    LogInfo("✓ HttpRequest integration works correctly\n");
}

int main() {
    LogInfo("========================================");
    LogInfo("HttpRouter Path Validation Tests");
    LogInfo("========================================\n");

    try {
        testValidPaths();
        testInvalidPaths();
        testDuplicateRoutes();
        testParameterExtraction();
        testEdgeCases();
        testHttpRequestIntegration();

        LogInfo("========================================");
        LogInfo("✓ ALL VALIDATION TESTS PASSED!");
        LogInfo("========================================");
        LogInfo("\nSummary:");
        LogInfo("- Path validation: ✅ Working");
        LogInfo("- Duplicate detection: ✅ Working");
        LogInfo("- Parameter extraction: ✅ Working");
        LogInfo("- HttpRequest integration: ✅ Working");
        LogInfo("- Edge cases: ✅ Working");
        return 0;
    } catch (const std::exception& e) {
        LogError("Test failed with exception: {}", e.what());
        return 1;
    }
}
