#ifndef GALAY_HTTPS_READER_H
#define GALAY_HTTPS_READER_H

#include <galay/kernel/async/Socket.h>
#include <galay/kernel/async/TimerGenerator.h>
#include <galay/kernel/coroutine/AsyncWaiter.hpp>
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include "HttpParams.hpp"
#include "galay/kernel/coroutine/CoSchedulerHandle.hpp"

namespace galay::http
{
    /**
     * @brief HTTPS Reader，支持 SSL socket 的 HTTP 协议读取
     * 
     * 这个类与 HttpReader 功能相同，但使用 AsyncSslSocket
     */
    class HttpsReader 
    {
    public:
        HttpsReader(AsyncSslSocket& socket, CoSchedulerHandle handle, HttpSettings params);
        
        AsyncResult<std::expected<HttpRequest, HttpError>> 
            getRequest(std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));
        
        AsyncResult<std::expected<HttpResponse, HttpError>> 
            getResponse(std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));
        
        AsyncResult<std::expected<void, HttpError>> 
            getChunkData(const std::function<void(std::string)>& callback,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));

    private:
        Coroutine<nil> readRequestHeader(std::shared_ptr<AsyncWaiter<HttpRequestHeader, HttpError>> waiter,
                                        std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));
        Coroutine<nil> readResponseHeader(std::shared_ptr<AsyncWaiter<HttpResponseHeader, HttpError>> waiter,
                                         std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));
        Coroutine<nil> readBody(std::shared_ptr<AsyncWaiter<std::string, HttpError>> waiter, 
                               size_t length,
                               std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));
        Coroutine<nil> readRequest(std::shared_ptr<AsyncWaiter<HttpRequest, HttpError>> waiter,
                                  std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));
        Coroutine<nil> readResponse(std::shared_ptr<AsyncWaiter<HttpResponse, HttpError>> waiter,
                                   std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));
        Coroutine<nil> readChunkBlock(std::shared_ptr<AsyncWaiter<void, HttpError>> waiter,
                                     const std::function<void(std::string)>& callback,
                                     std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));

    private:
        Buffer m_buffer;
        HttpSettings m_params;
        AsyncSslSocket& m_socket;
        CoSchedulerHandle m_handle;
    };
}

#endif // GALAY_HTTPS_READER_H

