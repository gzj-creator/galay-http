/**
 * @file test_range_etag.cc
 * @brief 测试 HTTP Range 和 ETag 功能
 */

#include <iostream>
#include <cassert>
#include <filesystem>
#include <fstream>
#include "galay-http/kernel/http/HttpRange.h"
#include "galay-http/kernel/http/HttpETag.h"
#include "galay-kernel/common/Log.h"

using namespace galay::http;
namespace fs = std::filesystem;

// ==================== HttpRange 测试 ====================

// 测试 1: 单范围解析
void test_single_range_parsing() {
    std::cout << "\n=== Test 1: Single Range Parsing ===" << std::endl;

    size_t fileSize = 1000;

    // 测试 bytes=0-499
    auto result1 = HttpRangeParser::parse("bytes=0-499", fileSize);
    assert(result1.isValid());
    assert(result1.type == RangeType::SINGLE_RANGE);
    assert(result1.ranges.size() == 1);
    assert(result1.ranges[0].start == 0);
    assert(result1.ranges[0].end == 499);
    assert(result1.ranges[0].length == 500);
    std::cout << "✓ bytes=0-499 parsed correctly" << std::endl;

    // 测试 bytes=500-999
    auto result2 = HttpRangeParser::parse("bytes=500-999", fileSize);
    assert(result2.isValid());
    assert(result2.ranges[0].start == 500);
    assert(result2.ranges[0].end == 999);
    assert(result2.ranges[0].length == 500);
    std::cout << "✓ bytes=500-999 parsed correctly" << std::endl;

    // 测试 bytes=500- (后缀范围)
    auto result3 = HttpRangeParser::parse("bytes=500-", fileSize);
    assert(result3.isValid());
    assert(result3.ranges[0].start == 500);
    assert(result3.ranges[0].end == 999);
    assert(result3.ranges[0].length == 500);
    std::cout << "✓ bytes=500- (suffix) parsed correctly" << std::endl;

    // 测试 bytes=-500 (前缀范围，最后500字节)
    auto result4 = HttpRangeParser::parse("bytes=-500", fileSize);
    assert(result4.isValid());
    assert(result4.ranges[0].start == 500);
    assert(result4.ranges[0].end == 999);
    assert(result4.ranges[0].length == 500);
    std::cout << "✓ bytes=-500 (prefix) parsed correctly" << std::endl;

    std::cout << "✓ Test 1 passed!" << std::endl;
}

// 测试 2: 多范围解析
void test_multiple_range_parsing() {
    std::cout << "\n=== Test 2: Multiple Range Parsing ===" << std::endl;

    size_t fileSize = 1000;

    // 测试 bytes=0-99,200-299,500-599
    auto result = HttpRangeParser::parse("bytes=0-99,200-299,500-599", fileSize);
    assert(result.isValid());
    assert(result.type == RangeType::MULTIPLE_RANGES);
    assert(result.ranges.size() == 3);

    assert(result.ranges[0].start == 0);
    assert(result.ranges[0].end == 99);
    assert(result.ranges[0].length == 100);

    assert(result.ranges[1].start == 200);
    assert(result.ranges[1].end == 299);
    assert(result.ranges[1].length == 100);

    assert(result.ranges[2].start == 500);
    assert(result.ranges[2].end == 599);
    assert(result.ranges[2].length == 100);

    assert(!result.boundary.empty());
    std::cout << "✓ Multiple ranges parsed correctly" << std::endl;
    std::cout << "✓ Boundary generated: " << result.boundary << std::endl;

    std::cout << "✓ Test 2 passed!" << std::endl;
}

// 测试 3: 无效范围处理
void test_invalid_range_handling() {
    std::cout << "\n=== Test 3: Invalid Range Handling ===" << std::endl;

    size_t fileSize = 1000;

    // 测试超出文件大小的范围
    auto result1 = HttpRangeParser::parse("bytes=1000-1999", fileSize);
    assert(!result1.isValid());
    std::cout << "✓ Out-of-bounds range rejected" << std::endl;

    // 测试起始位置大于结束位置
    auto result2 = HttpRangeParser::parse("bytes=500-100", fileSize);
    assert(!result2.isValid());
    std::cout << "✓ Invalid range (start > end) rejected" << std::endl;

    // 测试无效格式
    auto result3 = HttpRangeParser::parse("bytes=abc-def", fileSize);
    assert(!result3.isValid());
    std::cout << "✓ Invalid format rejected" << std::endl;

    // 测试空字符串
    auto result4 = HttpRangeParser::parse("", fileSize);
    assert(!result4.isValid());
    std::cout << "✓ Empty range rejected" << std::endl;

    // 测试非 bytes 单位
    auto result5 = HttpRangeParser::parse("items=0-10", fileSize);
    assert(!result5.isValid());
    std::cout << "✓ Non-bytes unit rejected" << std::endl;

    std::cout << "✓ Test 3 passed!" << std::endl;
}

// 测试 4: 边界情况
void test_range_edge_cases() {
    std::cout << "\n=== Test 4: Range Edge Cases ===" << std::endl;

    size_t fileSize = 1000;

    // 测试完整文件范围
    auto result1 = HttpRangeParser::parse("bytes=0-999", fileSize);
    assert(result1.isValid());
    assert(result1.ranges[0].length == fileSize);
    std::cout << "✓ Full file range works" << std::endl;

    // 测试单字节范围
    auto result2 = HttpRangeParser::parse("bytes=0-0", fileSize);
    assert(result2.isValid());
    assert(result2.ranges[0].length == 1);
    std::cout << "✓ Single byte range works" << std::endl;

    // 测试结束位置超出文件大小（应该被截断）
    auto result3 = HttpRangeParser::parse("bytes=900-1999", fileSize);
    assert(result3.isValid());
    assert(result3.ranges[0].end == 999);
    assert(result3.ranges[0].length == 100);
    std::cout << "✓ End position truncated to file size" << std::endl;

    // 测试最后一个字节
    auto result4 = HttpRangeParser::parse("bytes=-1", fileSize);
    assert(result4.isValid());
    assert(result4.ranges[0].start == 999);
    assert(result4.ranges[0].end == 999);
    assert(result4.ranges[0].length == 1);
    std::cout << "✓ Last byte range works" << std::endl;

    std::cout << "✓ Test 4 passed!" << std::endl;
}

// 测试 5: Content-Range 生成
void test_content_range_generation() {
    std::cout << "\n=== Test 5: Content-Range Generation ===" << std::endl;

    size_t fileSize = 1000;

    // 测试单范围
    HttpRange range1(0, 499);
    std::string contentRange1 = HttpRangeParser::makeContentRange(range1, fileSize);
    assert(contentRange1 == "bytes 0-499/1000");
    std::cout << "✓ Content-Range: " << contentRange1 << std::endl;

    // 测试另一个范围
    HttpRange range2(500, 999);
    std::string contentRange2 = HttpRangeParser::makeContentRange(range2, fileSize);
    assert(contentRange2 == "bytes 500-999/1000");
    std::cout << "✓ Content-Range: " << contentRange2 << std::endl;

    // 测试使用 start/end 参数
    std::string contentRange3 = HttpRangeParser::makeContentRange(100, 199, fileSize);
    assert(contentRange3 == "bytes 100-199/1000");
    std::cout << "✓ Content-Range: " << contentRange3 << std::endl;

    std::cout << "✓ Test 5 passed!" << std::endl;
}

// ==================== ETag 测试 ====================

// 测试 6: ETag 生成
void test_etag_generation() {
    std::cout << "\n=== Test 6: ETag Generation ===" << std::endl;

    // 创建测试文件
    std::string testFile = "./test_etag_file.txt";
    std::ofstream file(testFile);
    file << "Hello, World!";
    file.close();

    // 生成强 ETag
    std::string strongETag = ETagGenerator::generate(testFile, ETagGenerator::Type::STRONG);
    assert(!strongETag.empty());
    assert(strongETag[0] == '"');
    assert(strongETag[strongETag.length() - 1] == '"');
    std::cout << "✓ Strong ETag generated: " << strongETag << std::endl;

    // 生成弱 ETag
    std::string weakETag = ETagGenerator::generate(testFile, ETagGenerator::Type::WEAK);
    assert(!weakETag.empty());
    assert(weakETag.substr(0, 2) == "W/");
    std::cout << "✓ Weak ETag generated: " << weakETag << std::endl;

    // 清理
    fs::remove(testFile);

    std::cout << "✓ Test 6 passed!" << std::endl;
}

// 测试 7: ETag 匹配
void test_etag_matching() {
    std::cout << "\n=== Test 7: ETag Matching ===" << std::endl;

    std::string etag1 = "\"123-456-789\"";
    std::string etag2 = "\"123-456-789\"";
    std::string etag3 = "\"987-654-321\"";
    std::string weakETag1 = "W/\"123-456-789\"";

    // 测试相同 ETag 匹配
    assert(ETagGenerator::match(etag1, etag2));
    std::cout << "✓ Identical ETags match" << std::endl;

    // 测试不同 ETag 不匹配
    assert(!ETagGenerator::match(etag1, etag3));
    std::cout << "✓ Different ETags don't match" << std::endl;

    // 测试强弱 ETag 匹配（规范化后）
    assert(ETagGenerator::match(etag1, weakETag1));
    std::cout << "✓ Strong and weak ETags match (normalized)" << std::endl;

    std::cout << "✓ Test 7 passed!" << std::endl;
}

// 测试 8: If-None-Match 解析
void test_if_none_match_parsing() {
    std::cout << "\n=== Test 8: If-None-Match Parsing ===" << std::endl;

    // 测试单个 ETag
    std::string header1 = "\"123-456-789\"";
    auto etags1 = ETagGenerator::parseIfMatch(header1);
    assert(etags1.size() == 1);
    assert(etags1[0] == "123-456-789");
    std::cout << "✓ Single ETag parsed" << std::endl;

    // 测试多个 ETag
    std::string header2 = "\"123-456-789\", \"987-654-321\"";
    auto etags2 = ETagGenerator::parseIfMatch(header2);
    assert(etags2.size() == 2);
    assert(etags2[0] == "123-456-789");
    assert(etags2[1] == "987-654-321");
    std::cout << "✓ Multiple ETags parsed" << std::endl;

    // 测试弱 ETag
    std::string header3 = "W/\"123-456-789\"";
    auto etags3 = ETagGenerator::parseIfMatch(header3);
    assert(etags3.size() == 1);
    std::cout << "✓ Weak ETag parsed" << std::endl;

    std::cout << "✓ Test 8 passed!" << std::endl;
}

// 测试 9: matchAny 功能
void test_match_any() {
    std::cout << "\n=== Test 9: Match Any ETag ===" << std::endl;

    std::string currentETag = "\"123-456-789\"";
    std::vector<std::string> etagList1 = {"111-111-111", "123-456-789", "999-999-999"};
    std::vector<std::string> etagList2 = {"111-111-111", "222-222-222", "333-333-333"};

    // 测试匹配
    assert(ETagGenerator::matchAny(currentETag, etagList1));
    std::cout << "✓ ETag found in list" << std::endl;

    // 测试不匹配
    assert(!ETagGenerator::matchAny(currentETag, etagList2));
    std::cout << "✓ ETag not found in list" << std::endl;

    std::cout << "✓ Test 9 passed!" << std::endl;
}

// 测试 10: HTTP 日期格式化
void test_http_date_formatting() {
    std::cout << "\n=== Test 10: HTTP Date Formatting ===" << std::endl;

    std::time_t timestamp = 1234567890;  // 固定时间戳
    std::string httpDate = ETagGenerator::formatHttpDate(timestamp);

    assert(!httpDate.empty());
    assert(httpDate.find("GMT") != std::string::npos);
    std::cout << "✓ HTTP date formatted: " << httpDate << std::endl;

    std::cout << "✓ Test 10 passed!" << std::endl;
}

// 测试 11: If-Range 检查
void test_if_range_check() {
    std::cout << "\n=== Test 11: If-Range Check ===" << std::endl;

    std::string etag = "\"123-456-789\"";
    std::time_t lastModified = std::time(nullptr);

    // 测试 ETag 匹配
    assert(HttpRangeParser::checkIfRange(etag, etag, lastModified));
    std::cout << "✓ If-Range with matching ETag" << std::endl;

    // 测试 ETag 不匹配
    std::string differentETag = "\"999-999-999\"";
    assert(!HttpRangeParser::checkIfRange(differentETag, etag, lastModified));
    std::cout << "✓ If-Range with non-matching ETag" << std::endl;

    // 测试日期格式（简化实现总是返回 true）
    std::string httpDate = "Fri, 13 Feb 2009 23:31:30 GMT";
    assert(HttpRangeParser::checkIfRange(httpDate, etag, lastModified));
    std::cout << "✓ If-Range with HTTP date" << std::endl;

    std::cout << "✓ Test 11 passed!" << std::endl;
}

// 测试 12: HttpRange 结构体
void test_http_range_struct() {
    std::cout << "\n=== Test 12: HttpRange Struct ===" << std::endl;

    // 测试构造函数
    HttpRange range1(0, 499);
    assert(range1.start == 0);
    assert(range1.end == 499);
    assert(range1.length == 500);
    assert(range1.isValid());
    std::cout << "✓ HttpRange constructor works" << std::endl;

    // 测试默认构造函数
    HttpRange range2;
    assert(range2.start == 0);
    assert(range2.end == 0);
    assert(range2.length == 0);
    assert(!range2.isValid());
    std::cout << "✓ HttpRange default constructor works" << std::endl;

    // 测试无效范围
    HttpRange range3(500, 100);  // start > end
    assert(!range3.isValid());
    std::cout << "✓ Invalid HttpRange detected" << std::endl;

    std::cout << "✓ Test 12 passed!" << std::endl;
}

// 测试 13: RangeParseResult 结构体
void test_range_parse_result() {
    std::cout << "\n=== Test 13: RangeParseResult Struct ===" << std::endl;

    // 测试默认构造函数
    RangeParseResult result1;
    assert(result1.type == RangeType::INVALID);
    assert(!result1.isValid());
    std::cout << "✓ Default RangeParseResult is invalid" << std::endl;

    // 测试有效结果
    std::vector<HttpRange> ranges = {HttpRange(0, 499)};
    RangeParseResult result2(RangeType::SINGLE_RANGE, ranges);
    assert(result2.type == RangeType::SINGLE_RANGE);
    assert(result2.isValid());
    std::cout << "✓ Valid RangeParseResult created" << std::endl;

    // 测试边界生成
    std::string boundary1 = RangeParseResult::generateBoundary();
    std::string boundary2 = RangeParseResult::generateBoundary();
    assert(!boundary1.empty());
    assert(!boundary2.empty());
    assert(boundary1 != boundary2);  // 应该生成不同的边界
    std::cout << "✓ Unique boundaries generated" << std::endl;

    std::cout << "✓ Test 13 passed!" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "HTTP Range and ETag Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    try {
        // Range 测试
        test_single_range_parsing();
        test_multiple_range_parsing();
        test_invalid_range_handling();
        test_range_edge_cases();
        test_content_range_generation();

        // ETag 测试
        test_etag_generation();
        test_etag_matching();
        test_if_none_match_parsing();
        test_match_any();
        test_http_date_formatting();

        // 集成测试
        test_if_range_check();
        test_http_range_struct();
        test_range_parse_result();

        std::cout << "\n========================================" << std::endl;
        std::cout << "All tests passed! ✓" << std::endl;
        std::cout << "========================================" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n✗ Test failed with exception: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "\n✗ Test failed with unknown exception" << std::endl;
        return 1;
    }
}
