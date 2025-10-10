#include "HttpWriter.h"
#include "galay-http/utils/HttpLogger.h"

namespace galay::http
{
    HttpWriter::HttpWriter(AsyncTcpSocket &socket, TimerGenerator& generator, const HttpParams& params)
        : m_socket(socket), m_params(params), m_generator(generator)
    {
    }

    AsyncResult<std::expected<void, HttpError>> HttpWriter::send(HttpRequest &request, std::chrono::milliseconds timeout)
    {
        std::shared_ptr<AsyncWaiter<void, HttpError>> waiter = std::make_shared<AsyncWaiter<void, HttpError>>();
        waiter->appendTask(sendData(request.toString(), waiter, timeout));
        return waiter->wait();
    }

    AsyncResult<std::expected<void, HttpError>> HttpWriter::sendChunkHeader(HttpRequestHeader &header, std::chrono::milliseconds timeout)
    {
        std::shared_ptr<AsyncWaiter<void, HttpError>> waiter = std::make_shared<AsyncWaiter<void, HttpError>>();
        if(!header.isChunked()) {
            header.headerPairs().addHeaderPair("Transfer-Encoding", "chunked");
        }
        waiter->appendTask(sendData(header.toString(), waiter, timeout));
        return waiter->wait();
    }

    AsyncResult<std::expected<void, HttpError>> HttpWriter::sendChunkHeader(HttpRequestHeader &&header, std::chrono::milliseconds timeout)
    {
        std::shared_ptr<AsyncWaiter<void, HttpError>> waiter = std::make_shared<AsyncWaiter<void, HttpError>>();
        if(!header.isChunked()) {
            header.headerPairs().addHeaderPair("Transfer-Encoding", "chunked");
        }
        waiter->appendTask(sendData(header.toString(), waiter, timeout));
        return waiter->wait();
    }

    AsyncResult<std::expected<void, HttpError>> HttpWriter::reply(HttpResponse& response, std::chrono::milliseconds timeout)
    {
        std::shared_ptr<AsyncWaiter<void, HttpError>> waiter = std::make_shared<AsyncWaiter<void, HttpError>>();
        waiter->appendTask(sendData(response.toString(), waiter, timeout));
        return waiter->wait();
    }

    AsyncResult<std::expected<void, HttpError>> HttpWriter::replyChunkHeader(HttpResponseHeader &header, std::chrono::milliseconds timeout)
    {
        std::shared_ptr<AsyncWaiter<void, HttpError>> waiter = std::make_shared<AsyncWaiter<void, HttpError>>();
        if(!header.isChunked()) {
            header.headerPairs().addHeaderPair("Transfer-Encoding", "chunked");
        }
        waiter->appendTask(sendData(header.toString(), waiter, timeout));
        return waiter->wait();
    }

    AsyncResult<std::expected<void, HttpError>> HttpWriter::replyChunkHeader(HttpResponseHeader &&header, std::chrono::milliseconds timeout)
    {
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
        while (true)
        {
            std::expected<Bytes, CommonError> res;
            if(timeout <= std::chrono::milliseconds(0)) {
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
                waiter->notify(std::unexpected(HttpError(kHttpError_TcpSendError)));
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
        //length
        while(true)
        {
            std::expected<Bytes, CommonError> res;
            if(timeout <= std::chrono::milliseconds(0)) {
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
                HttpLogger::getInstance()->getLogger()->getSpdlogger()->error("[sendChunkData] {}", res.error().message());
                waiter->notify(std::unexpected(HttpError(kHttpError_TcpSendError)));
            }
        }
        waiter->notify({});
        co_return nil();
    }
}