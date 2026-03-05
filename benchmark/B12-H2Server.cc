/**
 * @file B12-H2Server.cc
 * @brief H2 (HTTP/2 over TLS) Echo 服务器压测程序
 */

#include "galay-http/kernel/http2/Http2Server.h"
#include "galay-http/kernel/http/HttpLog.h"
#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdlib>

#ifdef GALAY_HTTP_SSL_ENABLED

using namespace galay::http2;
using namespace galay::kernel;

static volatile bool g_running = true;
static const Http2HeaderField kStatus200{":status", "200"};
static const Http2HeaderField kContentTypeTextPlain{"content-type", "text/plain"};

void signalHandler(int) {
    g_running = false;
}

Coroutine handleStream(Http2Stream::ptr stream) {
    std::string body;

    bool end_stream = false;
    while (!end_stream) {
        auto frame_result = co_await stream->getFrame();
        if (!frame_result) {
            break;
        }

        auto frame = std::move(frame_result.value());
        if (!frame) {
            end_stream = true;
            break;
        }
        if (frame->isData()) {
            body.append(frame->asData()->data());
        }
        if (frame->isEndStream()) {
            end_stream = true;
        }
    }

    std::vector<Http2HeaderField> headers;
    headers.reserve(3);
    headers.push_back(kStatus200);
    headers.push_back(kContentTypeTextPlain);
    headers.push_back({"content-length", std::to_string(body.size())});

    stream->sendHeaders(headers, body.empty(), true);
    if (!body.empty()) {
        stream->sendData(body, true);
    }
    co_return;
}

int main(int argc, char* argv[]) {
    uint16_t port = 9443;
    int io_threads = 4;
    std::string cert_path = "../cert/test.crt";
    std::string key_path = "../cert/test.key";

    if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));
    if (argc > 2) io_threads = std::atoi(argv[2]);
    if (argc > 3) cert_path = argv[3];
    if (argc > 4) key_path = argv[4];

    galay::http::HttpLogger::disable();
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    std::cout << "========================================\n";
    std::cout << "H2 (HTTP/2 over TLS) Server Benchmark\n";
    std::cout << "========================================\n";
    std::cout << "Port: " << port << "\n";
    std::cout << "IO Threads: " << io_threads << "\n";
    std::cout << "Cert: " << cert_path << "\n";
    std::cout << "Key:  " << key_path << "\n";
    std::cout << "Press Ctrl+C to stop\n";
    std::cout << "========================================\n\n";

    try {
        H2Server server(H2ServerBuilder()
            .host("0.0.0.0")
            .port(port)
            .certPath(cert_path)
            .keyPath(key_path)
            .ioSchedulerCount(static_cast<size_t>(io_threads))
            .computeSchedulerCount(0)
            .maxConcurrentStreams(1000)
            .initialWindowSize(65535)
            .build());

        server.start(handleStream);
        std::cout << "Server started successfully!\n";
        std::cout << "Waiting for requests...\n\n";

        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "\nShutting down...\n";
        server.stop();
        std::cout << "Server stopped.\n";
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

#else

int main() {
    std::cout << "SSL support is not enabled.\n";
    std::cout << "Rebuild with -DGALAY_HTTP_ENABLE_SSL=ON\n";
    return 0;
}

#endif
