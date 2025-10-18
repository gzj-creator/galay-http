#include "HttpWriter.h"
#include "galay-http/utils/HttpLogger.h"
#include "galay-http/utils/HttpUtils.h"

namespace galay::http
{
    HttpWriter::HttpWriter(AsyncTcpSocket &socket, TimerGenerator& generator, const HttpSettings& params)
        : m_socket(socket), m_params(params), m_generator(generator)
    {
    }

    AsyncResult<std::expected<void, HttpError>> HttpWriter::send(HttpRequest &request, std::chrono::milliseconds timeout)
    {
        HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[HttpWriter] Send request");
        CLIENT_REQUEST_LOG(request.header().method(), request.header().uri());
        std::shared_ptr<AsyncWaiter<void, HttpError>> waiter = std::make_shared<AsyncWaiter<void, HttpError>>();
        waiter->appendTask(sendData(request.toString(), waiter, timeout));
        return waiter->wait();
    }

    AsyncResult<std::expected<void, HttpError>> HttpWriter::sendChunkHeader(HttpRequestHeader &header, std::chrono::milliseconds timeout)
    {
        CLIENT_REQUEST_LOG(header.method(), header.uri());
        std::shared_ptr<AsyncWaiter<void, HttpError>> waiter = std::make_shared<AsyncWaiter<void, HttpError>>();
        if(!header.isChunked()) {
            header.headerPairs().addHeaderPair("Transfer-Encoding", "chunked");
        }
        waiter->appendTask(sendData(header.toString(), waiter, timeout));
        return waiter->wait();
    }

    AsyncResult<std::expected<void, HttpError>> HttpWriter::reply(HttpResponse& response, std::chrono::milliseconds timeout)
    {
        HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[HttpWriter] Reply response");
        SERVER_RESPONSE_LOG(response.header().code());
        std::shared_ptr<AsyncWaiter<void, HttpError>> waiter = std::make_shared<AsyncWaiter<void, HttpError>>();
        waiter->appendTask(sendData(response.toString(), waiter, timeout));
        return waiter->wait();
    }

    AsyncResult<std::expected<void, HttpError>> HttpWriter::replyChunkHeader(HttpResponseHeader &header, std::chrono::milliseconds timeout)
    {
        SERVER_RESPONSE_LOG(header.code());
        std::shared_ptr<AsyncWaiter<void, HttpError>> waiter = std::make_shared<AsyncWaiter<void, HttpError>>();
        if(!header.isChunked()) {
            header.headerPairs().addHeaderPair("Transfer-Encoding", "chunked");
        }
        waiter->appendTask(sendData(header.toString(), waiter, timeout));
        return waiter->wait();
    }

    AsyncResult<std::expected<void, HttpError>> HttpWriter::replyChunkData(std::string_view chunk, bool is_last, std::chrono::milliseconds timeout)
    {
        std::shared_ptr<AsyncWaiter<void, HttpError>> waiter = std::make_shared<AsyncWaiter<void, HttpError>>();
        waiter->appendTask(sendChunkData(chunk, waiter, is_last, timeout));
        return waiter->wait();
    }

    AsyncResult<std::expected<void, HttpError>> HttpWriter::sendChunkData(std::string_view chunk, bool is_last, std::chrono::milliseconds timeout)
    {
        std::shared_ptr<AsyncWaiter<void, HttpError>> waiter = std::make_shared<AsyncWaiter<void, HttpError>>();
        waiter->appendTask(sendChunkData(chunk, waiter, is_last, timeout));
        return waiter->wait();
    }

    Coroutine<nil> HttpWriter::sendData(std::string data, std::shared_ptr<AsyncWaiter<void, HttpError>> waiter, std::chrono::milliseconds timeout)
    {
        auto str = std::move(data);
        auto bytes = Bytes::fromString(str);
        if(timeout == std::chrono::milliseconds(-1)) {
            timeout = m_params.send_timeout;
        }
        while (true)
        {
            std::expected<Bytes, CommonError> res;
            if(timeout < std::chrono::milliseconds(0)) {
                res = co_await m_socket.send(std::move(bytes));
            } else {
                auto temp = co_await m_generator.timeout<std::expected<Bytes, CommonError>>([&](){
                    return m_socket.send(std::move(bytes));
                }, timeout);
                if(!temp) {
                    HttpLogger::getInstance()->getLogger()->getSpdlogger()->error("[sendData] timeout");
                    waiter->notify(std::unexpected(HttpErrorCode::kHttpError_SendTimeOut));
                    co_return nil();
                }
                res = std::move(temp.value());
            }
            if(res) {
                bytes = std::move(res.value());
                if(bytes.empty()) {
                    break;
                }
            } else {
                HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[HttpWriter] Send failed: {}", res.error().message());
                waiter->notify(std::unexpected(HttpError(kHttpError_TcpSendError)));
                co_return nil();
            }
        }
        waiter->notify({});
        co_return nil();
    }

    Coroutine<nil> HttpWriter::sendChunkData(std::string_view chunk, \
        std::shared_ptr<AsyncWaiter<void, HttpError>> waiter, 
        bool is_last, std::chrono::milliseconds timeout)
    {
        std::ostringstream oss;
        oss << std::hex << chunk.size() << "\r\n" << chunk << "\r\n";
        if(is_last) {
            oss << "0\r\n\r\n";
        }
        std::string str = oss.str();
        Bytes bytes = Bytes::fromString(str);
        if(timeout == std::chrono::milliseconds(-1)) {
            timeout = m_params.send_timeout;
        }
        //length
        while(true)
        {
            std::expected<Bytes, CommonError> res;
            if(timeout < std::chrono::milliseconds(0)) {
                res = co_await m_socket.send(std::move(bytes));
            } else {
                auto temp = co_await m_generator.timeout<std::expected<Bytes, CommonError>>([&](){
                    return m_socket.send(std::move(bytes));
                }, timeout);
                if(!temp) {
                    HttpLogger::getInstance()->getLogger()->getSpdlogger()->error("[sendData] timeout");
                    waiter->notify(std::unexpected(HttpErrorCode::kHttpError_SendTimeOut));
                    co_return nil();
                }
                res = std::move(temp.value());
            }
            if(res) {
                bytes = std::move(res.value());
                if(bytes.empty()) {
                    break;
                }
            } else {
                HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[HttpWriter] Send chunk failed: {}", res.error().message());
                waiter->notify(std::unexpected(HttpError(kHttpError_TcpSendError)));
                co_return nil();
            }
        }
        waiter->notify({});
        co_return nil();
    }

#ifdef __linux__
    AsyncResult<std::expected<long, HttpError>> HttpWriter::sendfile(int file_fd, off_t offset, size_t length)
    {
        HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[HttpWriter] Sendfile {} bytes from offset {}", length, offset);
        auto waiter = std::make_shared<AsyncWaiter<long, HttpError>>();
        waiter->appendTask(sendfileInternal(file_fd, offset, length, waiter));
        return waiter->wait();
    }

    Coroutine<nil> HttpWriter::sendfileInternal(int file_fd, off_t offset, size_t length, 
                                                 std::shared_ptr<AsyncWaiter<long, HttpError>> waiter)
    {
        GHandle file_handle{.fd = file_fd};
        auto result = co_await m_socket.sendfile(file_handle, offset, length);
        
        if (!result) {
            HttpLogger::getInstance()->getLogger()->getSpdlogger()->error("[HttpWriter] Sendfile failed: {}", result.error().message());
            waiter->notify(std::unexpected(HttpError(kHttpError_TcpSendError)));
            co_return nil();
        }
        
        HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[HttpWriter] Sendfile successfully sent {} bytes", result.value());
        waiter->notify(result.value());
        co_return nil();
    }
#endif

    AsyncResult<std::expected<void, HttpError>> HttpWriter::upgradeToWebSocket(HttpRequest& request, std::chrono::milliseconds timeout)
    {
        HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[HttpWriter] Upgrade to WebSocket");
        auto& header = request.header();
        
        // 验证 Upgrade 头
        if (!header.headerPairs().hasKey("Upgrade")) {
            return {std::unexpected(HttpErrorCode::kHttpError_BadRequest)};
        }
        
        std::string upgrade_value = header.headerPairs().getValue("Upgrade");
        if (upgrade_value != "websocket") {
            return {std::unexpected(HttpErrorCode::kHttpError_BadRequest)};
        }
        
        // 验证 Connection 头
        if (!header.headerPairs().hasKey("Connection")) {
            return {std::unexpected(HttpErrorCode::kHttpError_BadRequest)};
        }
        
        // 验证 Sec-WebSocket-Key 头
        if (!header.headerPairs().hasKey("Sec-WebSocket-Key")) {
            return {std::unexpected(HttpErrorCode::kHttpError_BadRequest)};
        }
        
        std::string client_key = header.headerPairs().getValue("Sec-WebSocket-Key");
        if (client_key.empty()) {
            return {std::unexpected(HttpErrorCode::kHttpError_BadRequest)};
        }
        
        // 可选：验证 Sec-WebSocket-Version（应该是 13）
        if (header.headerPairs().hasKey("Sec-WebSocket-Version")) {
            std::string version = header.headerPairs().getValue("Sec-WebSocket-Version");
            if (version != "13") {
                return {std::unexpected(HttpErrorCode::kHttpError_VersionNotSupport)};
            }
        }
        
        // 创建 WebSocket 升级响应
        auto response = HttpUtils::createWebSocketUpgradeResponse(client_key);
        
        // 发送升级响应
        return reply(response, timeout);
    }
}