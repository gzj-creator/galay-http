#include "HttpReader.h"
#include "HttpUtils.h"
#include "HttpLogger.h"

namespace galay::http 
{
    HttpRequestReader::HttpRequestReader(AsyncTcpSocket &socket, Runtime& runtime, size_t id, HttpParams params)
        : m_socket(socket), m_id(id), m_runtime(runtime), m_params(params)
    {
    }

    AsyncResult<ValueWrapper<HttpRequest>> HttpRequestReader::getRequest()
    {
        m_runtime.schedule(readRequest(), m_id);
        return m_waiter.wait();
    }

    AsyncResult<ValueWrapper<std::string>> HttpRequestReader::getChunkHeader()
    {
        return AsyncResult<ValueWrapper<std::string>>();
    }

    Coroutine<nil> HttpRequestReader::readRequest()
    {
        HttpRequest request;
        HttpRequestHeader header;
        size_t header_size = 0;
        bool initial = true;
        auto error_rtn = [&](HttpErrorCode code) { 
            ValueWrapper<HttpRequest> res;
            makeValue(res, std::make_shared<HttpError>(code));
            m_waiter.notify(std::move(res));
        };

        while(header_size <= m_params.m_max_header_size) {
            ValueWrapper<Bytes> bytes_wrapper;
            if(initial) {
                bytes_wrapper = co_await m_socket.recv(m_params.m_peer_recv_length, true);
                initial = false;
            } else {
                bytes_wrapper = co_await m_socket.recv(m_params.m_peer_recv_length, false);
            }
            // recv error
            if(!bytes_wrapper.success()) {
                HttpLogger::getInstance()->getLogger()->getSpdlogger()->error("Recv error: {}", bytes_wrapper.getError()->message());
                error_rtn(kHttpError_BadRequest);
                co_return nil();
            }
            auto bytes = m_socket.getReadBytes();
            auto header_str = header.checkAndGetHeaderString(std::string_view(bytes.c_str(), bytes.size()));
            if(!header_str.empty()) {
                //header end
                auto error = header.fromString(header_str);
                if(error != HttpErrorCode::kHttpError_NoError) {
                    HttpLogger::getInstance()->getLogger()->getSpdlogger()->error("Http request header parse error: {}", HttpError(error).message());
                    error_rtn(kHttpError_BadRequest);
                    co_return nil();
                }
                break;
            } 
            header_size = bytes.size();
        }
        //header too long
        if( header_size > m_params.m_max_header_size) {
            HttpLogger::getInstance()->getLogger()->getSpdlogger()->error("Http request header too long");
            error_rtn(kHttpError_HeaderTooLong);
            co_return nil();
        }
        //chunk 不接收body
        if(header.isChunked()) {
            ValueWrapper<HttpRequest> res;
            request.setHeader(std::move(header));
            makeValue(res, std::move(request), nullptr);
            m_waiter.notify(std::move(res));
            co_return nil();
        }
        std::string body_length_str = header.headerPairs().getValue("Content-Length");
        // 没有Content-Length
        if(body_length_str.empty()) {
            HttpLogger::getInstance()->getLogger()->getSpdlogger()->error("Http request header not has Content-Length");
            error_rtn(kHttpError_BadRequest);
            co_return nil();
        }
        size_t body_length = 0;
        //body-length 转换为size_t
        try
        {
            body_length = std::stoull(body_length_str);
        }
        catch(const std::exception& e)
        {
            HttpLogger::getInstance()->getLogger()->getSpdlogger()->error("Convert body length to number failed, error: {}", e.what());
            error_rtn(kHttpError_BadRequest);
            co_return nil();
        }
        initial = true;
        //接收body
        while(body_length > 0)
        {
            ValueWrapper<Bytes> bytes_wrapper;
            if(initial) {
                bytes_wrapper = co_await m_socket.recv(body_length, true);
                initial = false;
            } else {
                bytes_wrapper = co_await m_socket.recv(body_length, false);
            }
            // recv error
            if(!bytes_wrapper.success()) {
                HttpLogger::getInstance()->getLogger()->getSpdlogger()->error("Recv error: {}", bytes_wrapper.getError()->message());
                error_rtn(kHttpError_BadRequest);
                co_return nil();
            }
            body_length -= bytes_wrapper.moveValue().size();
        }
        Bytes body_bytes = m_socket.getReadBytes();
        request.setBodyStr(body_bytes.toString());
        m_socket.clearReadBuffer();
        ValueWrapper<HttpRequest> result;
        makeValue(result, std::move(request), nullptr);
        m_waiter.notify(std::move(result));
        co_return nil();
    }
    
    Coroutine<nil> HttpRequestReader::readChunkHeader()
    {
        co_return nil();
    }

    Coroutine<nil> HttpRequestReader::readChunkBlock()
    {
        co_return nil();
    }
}