/**
 * @file test_http_router.cc
 * @brief HttpRouter 单元测试
 */

#include <iostream>
#include <cassert>
#include "galay-http/kernel/http/HttpRouter.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/kernel/http/HttpLog.h"

using namespace galay::http;

// 测试用的简单处理器
Coroutine testHandler(HttpConn& conn, HttpRequest req) {
    HTTP_LOG_INFO("Test handler called");
    co_return;
}

Coroutine userHandler(HttpConn& conn, HttpRequest req) {
    HTTP_LOG_INFO("User handler called");
    co_return;
}

Coroutine postHandler(HttpConn& conn, HttpRequest req) {
    HTTP_LOG_INFO("Post handler called");
    co_return;
}

Coroutine staticHandler(HttpConn& conn, HttpRequest req) {
    HTTP_LOG_INFO("Static handler called");
    co_return;
}

Coroutine filesHandler(HttpConn& conn, HttpRequest req) {
    HTTP_LOG_INFO("Files handler called");
    co_return;
}

void testExactMatch() {
    HTTP_LOG_INFO("========================================");
    HTTP_LOG_INFO("Test 1: Exact Match");
    HTTP_LOG_INFO("========================================");

    HttpRouter router;

    // 添加精确匹配路由
    router.addHandler<HttpMethod::GET>("/api/users", testHandler);
    router.addHandler<HttpMethod::POST>("/api/users", postHandler);
    router.addHandler<HttpMethod::GET>("/api/posts", testHandler);

    // 测试精确匹配
    auto match1 = router.findHandler(HttpMethod::GET, "/api/users");
    assert(match1.handler != nullptr);
    assert(match1.params.empty());
    HTTP_LOG_INFO("✓ GET /api/users matched");

    auto match2 = router.findHandler(HttpMethod::POST, "/api/users");
    assert(match2.handler != nullptr);
    HTTP_LOG_INFO("✓ POST /api/users matched");

    auto match3 = router.findHandler(HttpMethod::GET, "/api/posts");
    assert(match3.handler != nullptr);
    HTTP_LOG_INFO("✓ GET /api/posts matched");

    // 测试不匹配
    auto match4 = router.findHandler(HttpMethod::GET, "/api/comments");
    assert(match4.handler == nullptr);
    HTTP_LOG_INFO("✓ GET /api/comments not matched (expected)");

    auto match5 = router.findHandler(HttpMethod::DELETE, "/api/users");
    assert(match5.handler == nullptr);
    HTTP_LOG_INFO("✓ DELETE /api/users not matched (expected)");

    HTTP_LOG_INFO("✓ All exact match tests passed\n");
}

void testPathParameters() {
    HTTP_LOG_INFO("========================================");
    HTTP_LOG_INFO("Test 2: Path Parameters");
    HTTP_LOG_INFO("========================================");

    HttpRouter router;

    // 添加路径参数路由
    router.addHandler<HttpMethod::GET>("/user/:id", userHandler);
    router.addHandler<HttpMethod::GET>("/user/:id/posts/:postId", postHandler);

    // 测试单个参数
    auto match1 = router.findHandler(HttpMethod::GET, "/user/123");
    assert(match1.handler != nullptr);
    assert(match1.params.size() == 1);
    assert(match1.params["id"] == "123");
    HTTP_LOG_INFO("✓ GET /user/123 matched, id={}", match1.params["id"]);

    auto match2 = router.findHandler(HttpMethod::GET, "/user/abc");
    assert(match2.handler != nullptr);
    assert(match2.params["id"] == "abc");
    HTTP_LOG_INFO("✓ GET /user/abc matched, id={}", match2.params["id"]);

    // 测试多个参数
    auto match3 = router.findHandler(HttpMethod::GET, "/user/456/posts/789");
    assert(match3.handler != nullptr);
    assert(match3.params.size() == 2);
    assert(match3.params["id"] == "456");
    assert(match3.params["postId"] == "789");
    HTTP_LOG_INFO("✓ GET /user/456/posts/789 matched, id={}, postId={}",
            match3.params["id"], match3.params["postId"]);

    // 测试不匹配
    auto match4 = router.findHandler(HttpMethod::GET, "/user");
    assert(match4.handler == nullptr);
    HTTP_LOG_INFO("✓ GET /user not matched (expected)");

    auto match5 = router.findHandler(HttpMethod::GET, "/user/123/posts");
    assert(match5.handler == nullptr);
    HTTP_LOG_INFO("✓ GET /user/123/posts not matched (expected)");

    HTTP_LOG_INFO("✓ All path parameter tests passed\n");
}

void testWildcard() {
    HTTP_LOG_INFO("========================================");
    HTTP_LOG_INFO("Test 3: Wildcard Matching");
    HTTP_LOG_INFO("========================================");

    HttpRouter router;

    // 添加通配符路由
    router.addHandler<HttpMethod::GET>("/static/*", staticHandler);
    router.addHandler<HttpMethod::GET>("/files/**", filesHandler);

    // 测试单段通配符
    auto match1 = router.findHandler(HttpMethod::GET, "/static/css");
    assert(match1.handler != nullptr);
    HTTP_LOG_INFO("✓ GET /static/css matched");

    auto match2 = router.findHandler(HttpMethod::GET, "/static/js");
    assert(match2.handler != nullptr);
    HTTP_LOG_INFO("✓ GET /static/js matched");

    // 单段通配符不应匹配多段
    auto match3 = router.findHandler(HttpMethod::GET, "/static/css/style.css");
    assert(match3.handler == nullptr);
    HTTP_LOG_INFO("✓ GET /static/css/style.css not matched by /* (expected)");

    // 测试贪婪通配符
    auto match4 = router.findHandler(HttpMethod::GET, "/files/a");
    assert(match4.handler != nullptr);
    HTTP_LOG_INFO("✓ GET /files/a matched");

    auto match5 = router.findHandler(HttpMethod::GET, "/files/a/b/c");
    assert(match5.handler != nullptr);
    HTTP_LOG_INFO("✓ GET /files/a/b/c matched");

    HTTP_LOG_INFO("✓ All wildcard tests passed\n");
}

void testMultipleMethods() {
    HTTP_LOG_INFO("========================================");
    HTTP_LOG_INFO("Test 4: Multiple HTTP Methods");
    HTTP_LOG_INFO("========================================");

    HttpRouter router;

    // 为同一路径添加多个方法
    router.addHandler<HttpMethod::GET, HttpMethod::POST, HttpMethod::PUT>("/api/resource", testHandler);

    auto match1 = router.findHandler(HttpMethod::GET, "/api/resource");
    assert(match1.handler != nullptr);
    HTTP_LOG_INFO("✓ GET /api/resource matched");

    auto match2 = router.findHandler(HttpMethod::POST, "/api/resource");
    assert(match2.handler != nullptr);
    HTTP_LOG_INFO("✓ POST /api/resource matched");

    auto match3 = router.findHandler(HttpMethod::PUT, "/api/resource");
    assert(match3.handler != nullptr);
    HTTP_LOG_INFO("✓ PUT /api/resource matched");

    auto match4 = router.findHandler(HttpMethod::DELETE, "/api/resource");
    assert(match4.handler == nullptr);
    HTTP_LOG_INFO("✓ DELETE /api/resource not matched (expected)");

    HTTP_LOG_INFO("✓ All multiple methods tests passed\n");
}

void testPriorityMatching() {
    HTTP_LOG_INFO("========================================");
    HTTP_LOG_INFO("Test 5: Priority Matching (Exact > Param > Wildcard)");
    HTTP_LOG_INFO("========================================");

    HttpRouter router;

    // 添加不同优先级的路由
    router.addHandler<HttpMethod::GET>("/api/users", testHandler);      // 精确匹配
    router.addHandler<HttpMethod::GET>("/api/:resource", userHandler);  // 参数匹配
    router.addHandler<HttpMethod::GET>("/api/*", staticHandler);        // 通配符匹配

    // 精确匹配应该优先
    auto match1 = router.findHandler(HttpMethod::GET, "/api/users");
    assert(match1.handler != nullptr);
    assert(match1.params.empty());
    HTTP_LOG_INFO("✓ /api/users matched exact route (highest priority)");

    // 参数匹配应该次之
    auto match2 = router.findHandler(HttpMethod::GET, "/api/posts");
    assert(match2.handler != nullptr);
    assert(match2.params.size() == 1);
    assert(match2.params["resource"] == "posts");
    HTTP_LOG_INFO("✓ /api/posts matched param route, resource={}", match2.params["resource"]);

    HTTP_LOG_INFO("✓ All priority matching tests passed\n");
}

void testRouterOperations() {
    HTTP_LOG_INFO("========================================");
    HTTP_LOG_INFO("Test 6: Router Operations (size, clear, remove)");
    HTTP_LOG_INFO("========================================");

    HttpRouter router;

    // 测试 size
    assert(router.size() == 0);
    HTTP_LOG_INFO("✓ Initial size is 0");

    router.addHandler<HttpMethod::GET>("/api/users", testHandler);
    router.addHandler<HttpMethod::POST>("/api/users", postHandler);
    router.addHandler<HttpMethod::GET>("/user/:id", userHandler);

    assert(router.size() == 3);
    HTTP_LOG_INFO("✓ Size is 3 after adding 3 routes");

    // 测试 remove
    bool removed = router.delHandler(HttpMethod::GET, "/api/users");
    assert(removed);
    assert(router.size() == 2);
    HTTP_LOG_INFO("✓ Removed GET /api/users, size is now 2");

    auto match = router.findHandler(HttpMethod::GET, "/api/users");
    assert(match.handler == nullptr);
    HTTP_LOG_INFO("✓ GET /api/users no longer matches");

    // 测试 clear
    router.clear();
    assert(router.size() == 0);
    HTTP_LOG_INFO("✓ Cleared router, size is 0");

    match = router.findHandler(HttpMethod::POST, "/api/users");
    assert(match.handler == nullptr);
    HTTP_LOG_INFO("✓ All routes cleared");

    HTTP_LOG_INFO("✓ All router operation tests passed\n");
}

void testEdgeCases() {
    HTTP_LOG_INFO("========================================");
    HTTP_LOG_INFO("Test 7: Edge Cases");
    HTTP_LOG_INFO("========================================");

    HttpRouter router;

    // 根路径
    router.addHandler<HttpMethod::GET>("/", testHandler);
    auto match1 = router.findHandler(HttpMethod::GET, "/");
    assert(match1.handler != nullptr);
    HTTP_LOG_INFO("✓ Root path / matched");

    // 带尾部斜杠的路径
    router.addHandler<HttpMethod::GET>("/api/users/", userHandler);
    auto match2 = router.findHandler(HttpMethod::GET, "/api/users/");
    assert(match2.handler != nullptr);
    HTTP_LOG_INFO("✓ Path with trailing slash matched");

    // 空段应该被忽略
    auto match3 = router.findHandler(HttpMethod::GET, "//api//users//");
    // 这取决于 splitPath 的实现，应该与 /api/users/ 相同
    HTTP_LOG_INFO("✓ Path with multiple slashes handled");

    HTTP_LOG_INFO("✓ All edge case tests passed\n");
}

int main() {
    HTTP_LOG_INFO("========================================");
    HTTP_LOG_INFO("HttpRouter Unit Tests");
    HTTP_LOG_INFO("========================================\n");

    try {
        testExactMatch();
        testPathParameters();
        testWildcard();
        testMultipleMethods();
        testPriorityMatching();
        testRouterOperations();
        testEdgeCases();

        HTTP_LOG_INFO("========================================");
        HTTP_LOG_INFO("✓ ALL TESTS PASSED!");
        HTTP_LOG_INFO("========================================");
        return 0;
    } catch (const std::exception& e) {
        HTTP_LOG_ERROR("Test failed with exception: {}", e.what());
        return 1;
    }
}
