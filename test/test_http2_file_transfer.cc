// HTTP/2 文件传输测试
// 
// 演示如何在 HTTP/2 上传输文件
// 支持：
//  - 静态文件服务
//  - 大文件自动分片
//  - 多文件并发传输
//  - 文件下载进度监控
//
// 编译:
//   cd build && make test_http2_file_transfer
//
// 运行:
//   cd build/test && ./test_http2_file_transfer
//
// 测试:
//   curl -v --http2 https://localhost:8443/files/test.txt --insecure
//   curl -v --http2 https://localhost:8443/download/largefile.bin --insecure -o output.bin

#include "galay/common/Log.h"
#include "galay/kernel/runtime/Runtime.h"
#include "galay-http/server/Http2Server.h"
#include "galay-http/kernel/http2/Http2Connection.h"
#include "galay-http/kernel/http2/Http2Writer.h"
#include "galay-http/protoc/http2/Http2Hpack.h"
#include "galay-http/utils/HttpLogger.h"
#include "galay-http/utils/Http2DebugLog.h"
#include <galay/utils/SignalHandler.hpp>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <iostream>

using namespace galay;
using namespace galay::http;
namespace fs = std::filesystem;

// 文件传输信息
struct Http2FileTransferInfo {
    std::string file_path;
    std::string filename;
    size_t file_size;
    size_t bytes_sent;
    std::chrono::steady_clock::time_point start_time;
    
    double getProgress() const {
        return file_size > 0 ? (bytes_sent * 100.0) / file_size : 0.0;
    }
    
    double getSpeed() const {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - start_time).count();
        return elapsed > 0 ? (bytes_sent / (1024.0 * 1024.0)) / elapsed : 0.0;
    }
};

// 全局变量：活跃的文件传输
std::map<uint32_t, Http2FileTransferInfo> g_active_transfers;

// 获取 MIME 类型
std::string getMimeType(const std::string& filename) {
    std::string ext = fs::path(filename).extension().string();
    
    // 转换为小写
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    static const std::map<std::string, std::string> mime_types = {
        {".html", "text/html"},
        {".htm", "text/html"},
        {".css", "text/css"},
        {".js", "application/javascript"},
        {".json", "application/json"},
        {".xml", "application/xml"},
        {".txt", "text/plain"},
        {".md", "text/markdown"},
        
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".png", "image/png"},
        {".gif", "image/gif"},
        {".svg", "image/svg+xml"},
        {".ico", "image/x-icon"},
        {".webp", "image/webp"},
        
        {".mp4", "video/mp4"},
        {".webm", "video/webm"},
        {".ogv", "video/ogg"},
        
        {".mp3", "audio/mpeg"},
        {".ogg", "audio/ogg"},
        {".wav", "audio/wav"},
        
        {".pdf", "application/pdf"},
        {".zip", "application/zip"},
        {".tar", "application/x-tar"},
        {".gz", "application/gzip"},
        
        {".woff", "font/woff"},
        {".woff2", "font/woff2"},
        {".ttf", "font/ttf"},
        {".otf", "font/otf"}
    };
    
    auto it = mime_types.find(ext);
    return it != mime_types.end() ? it->second : "application/octet-stream";
}

// HEADERS 帧回调 - 处理文件请求
Coroutine<nil> onHeaders(Http2Connection& conn,
                          uint32_t stream_id,
                          const std::map<std::string, std::string>& headers,
                          bool end_stream)
{
    HTTP2_LOG_INFO("========================================");
    HTTP2_LOG_INFO("[HTTP/2 File Server] Stream {}: Received HEADERS", stream_id);
    
    // 解析请求
    std::string method, path;
    for (const auto& [key, value] : headers) {
        if (key == ":method") method = value;
        else if (key == ":path") path = value;
    }
    
    HTTP2_LOG_INFO("[HTTP/2 File Server] {} {}", method, path);
    
    if (method != "GET") {
        // 只支持 GET
        HTTP2_LOG_WARN("[HTTP/2 File Server] Method not allowed: {}", method);
        
        HpackEncoder encoder;
        std::vector<HpackHeaderField> error_headers = {
            {":status", "405"},
            {"content-type", "text/plain"},
            {"content-length", "18"}
        };
        std::string encoded = encoder.encodeHeaders(error_headers);
        
        auto writer = conn.getWriter({});
        co_await writer.sendHeaders(stream_id, encoded, false, true);
        co_await writer.sendData(stream_id, "Method Not Allowed", true);
        
        conn.streamManager().removeStream(stream_id);
        co_return nil();
    }
    
    // 构建文件路径
    std::string file_path;
    if (path.starts_with("/files/")) {
        // 示例文件目录
        file_path = "../../test/html" + path.substr(6);  // 移除 /files
    } else if (path.starts_with("/download/")) {
        // 下载目录（可以是任意位置）
        file_path = "./downloads" + path.substr(9);
    } else if (path == "/" || path == "/index.html") {
        // 默认主页
        file_path = "../../test/html/test_h2.html";
    } else {
        // 404
        HTTP2_LOG_WARN("[HTTP/2 File Server] File not found: {}", path);
        
        HpackEncoder encoder;
        std::vector<HpackHeaderField> error_headers = {
            {":status", "404"},
            {"content-type", "text/html"},
        };
        
        std::string error_body = R"(<!DOCTYPE html>
<html>
<head><title>404 Not Found</title></head>
<body>
    <h1>404 Not Found</h1>
    <p>The requested file was not found.</p>
    <p>Path: )" + path + R"(</p>
</body>
</html>)";
        
        error_headers.push_back({"content-length", std::to_string(error_body.size())});
        std::string encoded = encoder.encodeHeaders(error_headers);
        
        auto writer = conn.getWriter({});
        co_await writer.sendHeaders(stream_id, encoded, false, true);
        co_await writer.sendData(stream_id, error_body, true);
        
        conn.streamManager().removeStream(stream_id);
        co_return nil();
    }
    
    // 检查文件是否存在
    if (!fs::exists(file_path) || !fs::is_regular_file(file_path)) {
        HTTP2_LOG_ERROR("[HTTP/2 File Server] File not accessible: {}", file_path);
        
        HpackEncoder encoder;
        std::vector<HpackHeaderField> error_headers = {
            {":status", "404"},
            {"content-type", "text/plain"},
            {"content-length", "14"}
        };
        std::string encoded = encoder.encodeHeaders(error_headers);
        
        auto writer = conn.getWriter({});
        co_await writer.sendHeaders(stream_id, encoded, false, true);
        co_await writer.sendData(stream_id, "File Not Found", true);
        
        conn.streamManager().removeStream(stream_id);
        co_return nil();
    }
    
    // 获取文件信息
    size_t file_size = fs::file_size(file_path);
    std::string filename = fs::path(file_path).filename().string();
    std::string mime_type = getMimeType(filename);
    
    HTTP2_LOG_INFO("[HTTP/2 File Server] Serving file: {}", file_path);
    HTTP2_LOG_INFO("[HTTP/2 File Server] Size: {} bytes, MIME: {}", file_size, mime_type);
    
    // 记录传输信息
    Http2FileTransferInfo transfer_info;
    transfer_info.file_path = file_path;
    transfer_info.filename = filename;
    transfer_info.file_size = file_size;
    transfer_info.bytes_sent = 0;
    transfer_info.start_time = std::chrono::steady_clock::now();
    g_active_transfers[stream_id] = transfer_info;
    
    // 读取文件内容
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        HTTP2_LOG_ERROR("[HTTP/2 File Server] Failed to open file: {}", file_path);
        
        HpackEncoder encoder;
        std::vector<HpackHeaderField> error_headers = {
            {":status", "500"},
            {"content-type", "text/plain"},
            {"content-length", "21"}
        };
        std::string encoded = encoder.encodeHeaders(error_headers);
        
        auto writer = conn.getWriter({});
        co_await writer.sendHeaders(stream_id, encoded, false, true);
        co_await writer.sendData(stream_id, "Internal Server Error", true);
        
        g_active_transfers.erase(stream_id);
        conn.streamManager().removeStream(stream_id);
        co_return nil();
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string file_content = buffer.str();
    file.close();
    
    // 发送响应头
    HpackEncoder encoder;
    std::vector<HpackHeaderField> response_headers = {
        {":status", "200"},
        {"content-type", mime_type},
        {"content-length", std::to_string(file_size)},
        {"content-disposition", "inline; filename=\"" + filename + "\""},
        {"server", "galay-http2-file-server/1.0"},
        {"cache-control", "public, max-age=3600"},
        {"x-file-size", std::to_string(file_size)},
        {"access-control-allow-origin", "*"}
    };
    std::string encoded_headers = encoder.encodeHeaders(response_headers);
    
    auto writer = conn.getWriter({});
    
    // 发送 HEADERS
    auto headers_result = co_await writer.sendHeaders(stream_id, encoded_headers, false, true);
    if (!headers_result.has_value()) {
        HTTP2_LOG_ERROR("[HTTP/2 File Server] Failed to send headers: {}", 
                       headers_result.error().message());
        g_active_transfers.erase(stream_id);
        conn.streamManager().removeStream(stream_id);
        co_return nil();
    }
    
    HTTP2_LOG_INFO("[HTTP/2 File Server] Sending file data ({} bytes)...", file_size);
    
    // 发送文件内容（会自动分片）
    auto data_result = co_await writer.sendData(stream_id, file_content, true);
    if (!data_result.has_value()) {
        HTTP2_LOG_ERROR("[HTTP/2 File Server] Failed to send data: {}", 
                       data_result.error().message());
    } else {
        // 更新传输信息
        auto& transfer = g_active_transfers[stream_id];
        transfer.bytes_sent = file_size;
        
        HTTP2_LOG_INFO("[HTTP/2 File Server] ✅ File transfer complete: {}", filename);
        HTTP2_LOG_INFO("[HTTP/2 File Server] Speed: {:.2f} MB/s", transfer.getSpeed());
    }
    
    // 清理
    g_active_transfers.erase(stream_id);
    conn.streamManager().removeStream(stream_id);
    HTTP2_LOG_INFO("========================================");
    
    co_return nil();
}

// SETTINGS 帧回调
Coroutine<nil> onSettings(Http2Connection& conn,
                           const std::map<Http2SettingsId, uint32_t>& settings,
                           bool is_ack)
{
    if (!is_ack) {
        HTTP2_LOG_DEBUG("[HTTP/2 File Server] Received SETTINGS from client");
        for (const auto& [id, value] : settings) {
            if (id == Http2SettingsId::MAX_FRAME_SIZE) {
                HTTP2_LOG_INFO("[HTTP/2 File Server] Client max_frame_size: {}", value);
            }
        }
    }
    co_return nil();
}

// 错误回调
Coroutine<nil> onError(Http2Connection& conn, const Http2Error& error)
{
    HTTP2_LOG_ERROR("[HTTP/2 File Server] Error: {}", error.message());
    co_return nil();
}

int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << "  HTTP/2 文件传输服务器" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "监听地址: https://localhost:8443" << std::endl;
    std::cout << "协议: HTTP/2 over TLS (h2)" << std::endl;
    std::cout << "功能: 静态文件服务 + 大文件自动分片" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    // 检查证书
    std::ifstream cert_file("server.crt");
    std::ifstream key_file("server.key");
    
    if (!cert_file.good() || !key_file.good()) {
        std::cerr << "错误：SSL 证书文件不存在！" << std::endl;
        std::cerr << "请先生成证书：" << std::endl;
        std::cerr << "openssl req -x509 -newkey rsa:4096 -keyout server.key -out server.crt -days 365 -nodes -subj \"/CN=localhost\"" << std::endl;
        return 1;
    }
    
    // 设置日志级别
    HttpLogger::getInstance()->getLogger()->getSpdlogger()->set_level(spdlog::level::level_enum::info);
    
    // 创建运行时
    RuntimeBuilder runtimebuilder;
    auto runtime = runtimebuilder.build();
    runtime.start();
    
    // 配置 HTTP/2 回调
    Http2Callbacks callbacks;
    callbacks.on_headers = onHeaders;
    callbacks.on_settings = onSettings;
    callbacks.on_error = onError;
    
    // 配置 HTTP/2 参数（支持大文件）
    Http2Settings params;
    params.max_frame_size = 16384;  // 16KB（默认值）
    params.initial_window_size = 1048576;  // 1MB 窗口
    params.connection_window_size = 10485760;  // 10MB 连接窗口
    
    HTTP2_LOG_INFO("HTTP/2 Settings:");
    HTTP2_LOG_INFO("  max_frame_size: {} bytes", params.max_frame_size);
    HTTP2_LOG_INFO("  initial_window_size: {} bytes", params.initial_window_size);
    HTTP2_LOG_INFO("  connection_window_size: {} bytes", params.connection_window_size);
    
    // 创建 HTTP/2 服务器
    Http2Server server = Http2ServerBuilder("server.crt", "server.key")
                            .addListen(Host("0.0.0.0", 8443))
                            .build();
    
    // 信号处理
    utils::SignalHandler::setSignalHandler<SIGINT>([&server](int signal) {
        HTTP2_LOG_INFO("接收到停止信号，关闭服务器...");
        server.stop();
    });
    
    std::cout << "服务器启动成功！" << std::endl;
    std::cout << std::endl;
    std::cout << "可用端点：" << std::endl;
    std::cout << "  /               - 主页" << std::endl;
    std::cout << "  /files/*        - 静态文件（来自 test/html/）" << std::endl;
    std::cout << "  /download/*     - 下载文件（来自 ./downloads/）" << std::endl;
    std::cout << std::endl;
    std::cout << "测试命令：" << std::endl;
    std::cout << "  # 下载主页" << std::endl;
    std::cout << "  curl -v --http2 https://localhost:8443/ --insecure" << std::endl;
    std::cout << std::endl;
    std::cout << "  # 下载测试文件" << std::endl;
    std::cout << "  curl -v --http2 https://localhost:8443/files/test_h2.html --insecure -o test.html" << std::endl;
    std::cout << std::endl;
    std::cout << "  # 并发下载多个文件（测试多路复用）" << std::endl;
    std::cout << "  curl --http2 https://localhost:8443/files/test1.html --insecure -o t1.html & \\" << std::endl;
    std::cout << "  curl --http2 https://localhost:8443/files/test2.html --insecure -o t2.html &" << std::endl;
    std::cout << std::endl;
    std::cout << "注意：" << std::endl;
    std::cout << "  - 大文件会自动分片（每个分片最大 16KB）" << std::endl;
    std::cout << "  - 支持 HTTP/2 多路复用，可以同时下载多个文件" << std::endl;
    std::cout << "  - 传输速度和进度会在日志中显示" << std::endl;
    std::cout << "========================================" << std::endl;
    
    // 启动服务器
    server.run(runtime, callbacks, params);
    server.wait();
    
    HTTP2_LOG_INFO("服务器已停止");
    return 0;
}

