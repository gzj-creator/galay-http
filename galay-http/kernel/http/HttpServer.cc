#include "HttpServer.h"
#include "HttpLog.h"

namespace galay::http
{

HttpServer::HttpServer(IOScheduler* scheduler, const HttpServerConfig& config)
    : m_scheduler(scheduler)
    , m_config(config)
    , m_handler(nullptr)
    , m_listener(nullptr)
    , m_running(false)
{
}

HttpServer::~HttpServer()
{
    stop();
}

bool HttpServer::start()
{
    if (m_running.load()) {
        HTTP_LOG_WARN("server already running");
        return false;
    }

    if (!m_handler) {
        HTTP_LOG_ERROR("handler not set");
        return false;
    }

    // 创建监听socket
    m_listener = std::make_unique<TcpSocket>(IPType::IPV4);

    // 设置socket选项
    auto reuse_result = m_listener->option().handleReuseAddr();
    if (!reuse_result) {
        HTTP_LOG_ERROR("failed to set reuse addr: {}", reuse_result.error().message());
        return false;
    }

    auto nonblock_result = m_listener->option().handleNonBlock();
    if (!nonblock_result) {
        HTTP_LOG_ERROR("failed to set non-block: {}", nonblock_result.error().message());
        return false;
    }

    // 绑定地址
    Host bind_host(IPType::IPV4, m_config.host, m_config.port);
    auto bind_result = m_listener->bind(bind_host);
    if (!bind_result) {
        HTTP_LOG_ERROR("failed to bind {}:{}: {}", m_config.host, m_config.port, bind_result.error().message());
        return false;
    }

    // 开始监听
    auto listen_result = m_listener->listen(m_config.backlog);
    if (!listen_result) {
        HTTP_LOG_ERROR("failed to listen: {}", listen_result.error().message());
        return false;
    }

    m_running.store(true);
    HTTP_LOG_INFO("HTTP server started on {}:{}", m_config.host, m_config.port);

    // 启动服务器循环协程
    m_scheduler->spawn(serverLoop());

    return true;
}

void HttpServer::stop()
{
    if (!m_running.load()) {
        return;
    }

    m_running.store(false);
    HTTP_LOG_INFO("HTTP server stopping...");

    // 关闭监听socket
    if (m_listener) {
        m_listener.reset();
    }

    HTTP_LOG_INFO("HTTP server stopped");
}

Coroutine HttpServer::serverLoop()
{
    while (m_running.load()) {
        // 接受新连接
        Host client_host;
        auto accept_result = co_await m_listener->accept(&client_host);

        if (!accept_result) {
            if (m_running.load()) {
                HTTP_LOG_ERROR("accept failed: {}", accept_result.error().message());
            }
            continue;
        }

        HTTP_LOG_INFO("client connected from {}:{}", client_host.ip(), client_host.port());

        // 创建客户端socket并启动处理协程
        TcpSocket client_socket(accept_result.value());
        client_socket.option().handleNonBlock();

        m_scheduler->spawn(handleConnection(std::move(client_socket)));
    }

    co_return;
}

Coroutine HttpServer::handleConnection(TcpSocket socket)
{
    // 创建HttpConn
    HttpConn conn(std::move(socket), m_config.reader_setting);

    // 处理连接（在当前协程中执行）
    HttpRequest request;
    HttpResponse response;

    while (true) {
        // 重置请求和响应
        request.reset();

        // 读取HTTP请求
        bool request_complete = false;
        while (!request_complete) {
            // 获取可写iovec用于readv
            auto write_iovecs = conn.m_ring_buffer.getWriteIovecs();
            if (write_iovecs.empty()) {
                HTTP_LOG_DEBUG("ring buffer full, closing connection");
                co_await conn.m_socket.close();
                co_return;
            }

            // 异步读取数据
            auto readv_awaitable = conn.m_socket.readv(std::move(write_iovecs));
            auto result = co_await conn.m_reader.getRequest(request, std::move(readv_awaitable));

            if (!result) {
                // 解析错误
                auto& error = result.error();
                HTTP_LOG_DEBUG("request parse error: {}", error.message());

                if (error.code() == kConnectionClose) {
                    co_await conn.m_socket.close();
                    co_return;
                }

                // 发送错误响应
                HttpResponseHeader error_header;
                error_header.version() = HttpVersion::HttpVersion_1_1;
                error_header.code() = error.toHttpStatusCode();
                error_header.headerPairs().addHeaderPair("Content-Length", "0");
                error_header.headerPairs().addHeaderPair("Connection", "close");

                response.setHeader(std::move(error_header));
                response.setBodyStr("");

                std::string response_str = response.toString();
                std::vector<struct iovec> iovecs(1);
                iovecs[0].iov_base = const_cast<char*>(response_str.data());
                iovecs[0].iov_len = response_str.size();

                co_await conn.m_writer.sendResponse(response, conn.m_socket.writev(std::move(iovecs)));
                co_await conn.m_socket.close();
                co_return;
            }

            request_complete = result.value();
        }

        HTTP_LOG_DEBUG("request received: {} {}",
                     static_cast<int>(request.header().method()),
                     request.header().uri());

        // 调用用户处理函数
        m_handler(request, response);

        // 发送响应
        std::string response_str = response.toString();
        std::vector<struct iovec> iovecs(1);
        iovecs[0].iov_base = const_cast<char*>(response_str.data());
        iovecs[0].iov_len = response_str.size();

        auto send_result = co_await conn.m_writer.sendResponse(response, conn.m_socket.writev(std::move(iovecs)));
        if (!send_result) {
            HTTP_LOG_DEBUG("send response failed: {}", send_result.error().message());
            co_await conn.m_socket.close();
            co_return;
        }

        HTTP_LOG_DEBUG("response sent: {} bytes", send_result.value());

        // 检查是否需要关闭连接
        std::string connection_header = request.header().headerPairs().getValue("Connection");
        if (!connection_header.empty() && connection_header == "close") {
            co_await conn.m_socket.close();
            co_return;
        }

        // HTTP/1.0默认关闭连接
        if (request.header().version() == HttpVersion::HttpVersion_1_0) {
            co_await conn.m_socket.close();
            co_return;
        }
    }

    co_return;
}

} // namespace galay::http
