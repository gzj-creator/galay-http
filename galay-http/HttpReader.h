#ifndef GALAY_HTTP_READER_H
#define GALAY_HTTP_READER_H 

#include <galay/kernel/async/Socket.h>
#include <galay/kernel/coroutine/AsyncWaiter.hpp>
#include "HttpRequest.h"

namespace galay::http
{
    struct HttpParams {
        size_t m_peer_recv_length = DEFAULT_HTTP_PEER_RECV_SIZE;
        size_t m_max_header_size = DEFAULT_HTTP_MAX_HEADER_SIZE;
    };

    //不支持跨线程协程调用
    class HttpRequestReader 
    {
    public:
        HttpRequestReader(AsyncTcpSocket& socket, Runtime& runtime, size_t id, HttpParams params);
        /*
            获取完整请求
            error code:
                kHttpError_HeaderTooLong
                kHttpError_BadRequest
        */
        AsyncResult<std::expected<HttpRequest, HttpError>> getRequest();
        AsyncResult<std::expected<HttpRequestHeader, HttpError>> getChunkHeader();
        AsyncResult<std::expected<std::string, HttpError>> getChunkBlock();

    private:
        Coroutine<nil> readRequest();
        Coroutine<nil> readChunkHeader();
        Coroutine<nil> readChunkBlock();
    private:
        AsyncTcpSocket& m_socket;
        size_t m_id;
        Runtime& m_runtime;
        HttpParams m_params;
    };

    class HttpResponseReader 
    {
    public:
        HttpResponseReader(AsyncTcpSocket& socket, Runtime& runtime, size_t id, HttpParams params);
    private:
        Coroutine<nil> readResponse();
    
    };

}

#endif