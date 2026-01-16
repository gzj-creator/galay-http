#include "HttpConn.h"
#include "HttpLog.h"

namespace galay::http
{

HttpConn::HttpConn(TcpSocket&& socket, const HttpReaderSetting& setting)
    : m_socket(std::move(socket))
    , m_ring_buffer(8192)  // 8KB buffer
    , m_setting(setting)
    , m_reader(m_ring_buffer, m_setting)
    , m_writer()
{
}

Coroutine HttpConn::handle(HttpHandler handler)
{
    HttpRequest request;
    HttpResponse response;

    while (true) {
        // 重置请求和响应
        request.reset();

        // 读取HTTP请求
        bool request_complete = false;
        while (!request_complete) {
            // 获取可写iovec用于readv
            auto write_iovecs = m_ring_buffer.getWriteIovecs();
            if (write_iovecs.empty()) {
                // buffer满了，可能是恶意请求
                HTTP_LOG_DEBUG("ring buffer full, closing connection");
                co_await m_socket.close();
                co_return;
            }

            // 异步读取数据
            auto readv_awaitable = m_socket.readv(std::move(write_iovecs));
            auto result = co_await m_reader.getRequest(request, std::move(readv_awaitable));

            if (!result) {
                // 解析错误
                auto& error = result.error();
                HTTP_LOG_DEBUG("request parse error: {}", error.message());

                // 如果是连接关闭，直接返回
                if (error.code() == kConnectionClose) {
                    co_await m_socket.close();
                    co_return;
                }

                // 其他错误，发送错误响应
                HttpResponseHeader error_header;
                error_header.version() = HttpVersion::HttpVersion_1_1;
                error_header.code() = error.toHttpStatusCode();
                error_header.headerPairs().addHeaderPair("Content-Length", "0");
                error_header.headerPairs().addHeaderPair("Connection", "close");

                response.setHeader(std::move(error_header));
                response.setBodyStr("");

                // 发送错误响应
                std::string response_str = response.toString();
                std::vector<struct iovec> iovecs(1);
                iovecs[0].iov_base = const_cast<char*>(response_str.data());
                iovecs[0].iov_len = response_str.size();

                auto send_result = co_await m_writer.sendResponse(response, m_socket.writev(std::move(iovecs)));

                co_await m_socket.close();
                co_return;
            }

            request_complete = result.value();
        }

        HTTP_LOG_DEBUG("request received: {} {}",
                     static_cast<int>(request.header().method()),
                     request.header().uri());

        // 调用用户处理函数
        handler(request, response);

        // 发送响应
        std::string response_str = response.toString();
        std::vector<struct iovec> iovecs(1);
        iovecs[0].iov_base = const_cast<char*>(response_str.data());
        iovecs[0].iov_len = response_str.size();

        auto send_result = co_await m_writer.sendResponse(response, m_socket.writev(std::move(iovecs)));
        if (!send_result) {
            HTTP_LOG_DEBUG("send response failed: {}", send_result.error().message());
            co_await m_socket.close();
            co_return;
        }

        HTTP_LOG_DEBUG("response sent: {} bytes", send_result.value());

        // 检查是否需要关闭连接
        std::string connection_header = request.header().headerPairs().getValue("Connection");
        if (!connection_header.empty() && connection_header == "close") {
            co_await m_socket.close();
            co_return;
        }

        // HTTP/1.0默认关闭连接
        if (request.header().version() == HttpVersion::HttpVersion_1_0) {
            co_await m_socket.close();
            co_return;
        }
    }
}

Coroutine HttpConn::close()
{
    HTTP_LOG_DEBUG("closing connection");
    co_await m_socket.close();
}

} // namespace galay::http
