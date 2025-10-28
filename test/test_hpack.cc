/**
 * @file test_hpack.cc
 * @brief HPACK 编码/解码测试
 * 
 * 测试 HPACK 头部压缩功能，包括：
 * - 哈夫曼编码/解码
 * - 静态表
 * - 动态表
 * - HPACK 编码/解码
 */

#include "galay-http/protoc/http2/Http2Hpack.h"
#include "galay-http/protoc/http2/Http2Huffman.h"
#include "galay-http/protoc/http2/Http2HpackTable.h"
#include "galay-http/protoc/http2/Http2Frame.h"
#include <iostream>
#include <cassert>
#include <iomanip>

using namespace galay::http;

void printHex(const std::string& data, const std::string& label = "") {
    if (!label.empty()) {
        std::cout << label << ": ";
    }
    for (unsigned char c : data) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c) << " ";
    }
    std::cout << std::dec << std::endl;
}

void testHuffman() {
    std::cout << "====== 测试哈夫曼编码 ======" << std::endl;
    
    // 测试 1: 简单字符串
    std::string test1 = "www.example.com";
    std::cout << "原始: " << test1 << std::endl;
    
    auto encoded1 = Http2Huffman::encode(test1);
    printHex(encoded1, "编码");
    
    auto decoded1 = Http2Huffman::decode(
        reinterpret_cast<const uint8_t*>(encoded1.data()), 
        encoded1.size()
    );
    
    if (decoded1.has_value()) {
        std::cout << "解码: " << decoded1.value() << std::endl;
        assert(decoded1.value() == test1);
        std::cout << "✓ 哈夫曼编码/解码成功" << std::endl;
    } else {
        std::cout << "✗ 哈夫曼解码失败" << std::endl;
    }
    
    // 测试 2: 空字符串
    std::string test2 = "";
    auto encoded2 = Http2Huffman::encode(test2);
    auto decoded2 = Http2Huffman::decode(
        reinterpret_cast<const uint8_t*>(encoded2.data()),
        encoded2.size()
    );
    assert(decoded2.has_value() && decoded2.value() == test2);
    std::cout << "✓ 空字符串测试通过" << std::endl;
    
    std::cout << std::endl;
}

void testStaticTable() {
    std::cout << "====== 测试静态表 ======" << std::endl;
    
    // 测试索引 1: ":authority"
    auto field1 = HpackStaticTable::get(1);
    assert(field1.has_value());
    assert(field1.value().name == ":authority");
    assert(field1.value().value == "");
    std::cout << "✓ 索引 1: :authority" << std::endl;
    
    // 测试索引 2: ":method GET"
    auto field2 = HpackStaticTable::get(2);
    assert(field2.has_value());
    assert(field2.value().name == ":method");
    assert(field2.value().value == "GET");
    std::cout << "✓ 索引 2: :method GET" << std::endl;
    
    // 测试查找
    size_t index = HpackStaticTable::findExactMatch(":method", "POST");
    assert(index == 3);
    std::cout << "✓ 查找 :method POST -> 索引 3" << std::endl;
    
    size_t name_index = HpackStaticTable::findNameMatch(":path");
    assert(name_index == 4);
    std::cout << "✓ 查找名称 :path -> 索引 4" << std::endl;
    
    std::cout << std::endl;
}

void testDynamicTable() {
    std::cout << "====== 测试动态表 ======" << std::endl;
    
    HpackDynamicTable table(256);
    
    // 添加条目
    table.add("custom-key", "custom-value");
    assert(table.size() == 1);
    std::cout << "✓ 添加条目: custom-key: custom-value" << std::endl;
    
    // 获取条目
    auto field = table.get(1);
    assert(field.has_value());
    assert(field.value().name == "custom-key");
    assert(field.value().value == "custom-value");
    std::cout << "✓ 获取条目成功" << std::endl;
    
    // 查找
    size_t index = table.findExactMatch("custom-key", "custom-value");
    assert(index == 1);
    std::cout << "✓ 查找成功: 索引 1" << std::endl;
    
    // 测试驱逐（添加超过最大大小的条目）
    for (int i = 0; i < 10; ++i) {
        table.add("key-" + std::to_string(i), "value-" + std::to_string(i));
    }
    std::cout << "✓ 添加多个条目，当前大小: " << table.currentSize() 
              << ", 条目数: " << table.size() << std::endl;
    assert(table.currentSize() <= 256);
    
    std::cout << std::endl;
}

void testHpackEncoder() {
    std::cout << "====== 测试 HPACK 编码 ======" << std::endl;
    
    HpackEncoder encoder;
    
    // 测试 1: 使用静态表的索引头部
    std::vector<HpackHeaderField> headers1 = {
        {":method", "GET"},
        {":path", "/"}
    };
    
    auto encoded1 = encoder.encodeHeaders(headers1, false);
    printHex(encoded1, "编码结果");
    std::cout << "✓ 编码了 " << headers1.size() << " 个头部" << std::endl;
    
    // 测试 2: 使用哈夫曼编码
    std::vector<HpackHeaderField> headers2 = {
        {":authority", "www.example.com"},
        {":method", "GET"},
        {":path", "/index.html"}
    };
    
    auto encoded2 = encoder.encodeHeaders(headers2, true);
    printHex(encoded2, "哈夫曼编码");
    std::cout << "✓ 使用哈夫曼编码了 " << headers2.size() << " 个头部" << std::endl;
    
    std::cout << std::endl;
}

void testHpackDecoder() {
    std::cout << "====== 测试 HPACK 解码 ======" << std::endl;
    
    HpackEncoder encoder;
    HpackDecoder decoder;
    
    // 准备测试数据
    std::vector<HpackHeaderField> original_headers = {
        {":method", "GET"},
        {":path", "/"},
        {":authority", "www.example.com"},
        {"content-type", "text/html"},
        {"custom-header", "custom-value"}
    };
    
    std::cout << "原始头部:" << std::endl;
    for (const auto& h : original_headers) {
        std::cout << "  " << h.name << ": " << h.value << std::endl;
    }
    
    // 编码
    auto encoded = encoder.encodeHeaders(original_headers, true);
    printHex(encoded, "编码数据");
    
    // 解码
    auto decoded = decoder.decodeHeaders(
        reinterpret_cast<const uint8_t*>(encoded.data()),
        encoded.size()
    );
    
    if (decoded.has_value()) {
        std::cout << "解码头部:" << std::endl;
        for (const auto& h : decoded.value()) {
            std::cout << "  " << h.name << ": " << h.value << std::endl;
        }
        
        // 验证
        assert(decoded.value().size() == original_headers.size());
        for (size_t i = 0; i < original_headers.size(); ++i) {
            assert(decoded.value()[i].name == original_headers[i].name);
            assert(decoded.value()[i].value == original_headers[i].value);
        }
        std::cout << "✓ HPACK 编码/解码测试通过" << std::endl;
    } else {
        std::cout << "✗ 解码失败: " << decoded.error().message() << std::endl;
    }
    
    std::cout << std::endl;
}

void testHttp2HeadersFrame() {
    std::cout << "====== 测试 HTTP/2 HEADERS 帧 ======" << std::endl;
    
    HpackEncoder encoder;
    HpackDecoder decoder;
    
    // 创建头部列表
    std::vector<HpackHeaderField> headers = {
        {":method", "POST"},
        {":path", "/api/data"},
        {":scheme", "https"},
        {":authority", "api.example.com"},
        {"content-type", "application/json"},
        {"content-length", "1234"}
    };
    
    std::cout << "创建 HEADERS 帧，包含 " << headers.size() << " 个头部" << std::endl;
    
    // 使用 fromHeaders 创建帧
    auto frame = Http2HeadersFrame::fromHeaders(1, headers, encoder, false, true);
    
    std::cout << "✓ HEADERS 帧创建成功" << std::endl;
    std::cout << "  流 ID: " << frame.header().stream_id << std::endl;
    std::cout << "  头部块大小: " << frame.headerBlock().size() << " 字节" << std::endl;
    
    // 序列化帧
    auto serialized = frame.serialize();
    std::cout << "  序列化大小: " << serialized.size() << " 字节" << std::endl;
    
    // 解码头部
    auto decoded_headers = frame.decodeHeaders(decoder);
    
    if (decoded_headers.has_value()) {
        std::cout << "✓ 解码成功，头部列表:" << std::endl;
        for (const auto& h : decoded_headers.value()) {
            std::cout << "  " << h.name << ": " << h.value << std::endl;
        }
        
        // 验证
        assert(decoded_headers.value().size() == headers.size());
        std::cout << "✓ HTTP/2 HEADERS 帧测试通过" << std::endl;
    } else {
        std::cout << "✗ 解码失败: " << decoded_headers.error().message() << std::endl;
    }
    
    std::cout << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "       HPACK 压缩测试套件" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    try {
        testHuffman();
        testStaticTable();
        testDynamicTable();
        testHpackEncoder();
        testHpackDecoder();
        testHttp2HeadersFrame();
        
        std::cout << "========================================" << std::endl;
        std::cout << "          所有测试通过！✓" << std::endl;
        std::cout << "========================================" << std::endl;
        
        return 0;
    } catch (const std::exception& e) {
        std::cout << "异常: " << e.what() << std::endl;
        return 1;
    }
}

