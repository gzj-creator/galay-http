#ifndef GALAY_HTTPS_WRITER_H
#define GALAY_HTTPS_WRITER_H 

#include <galay/kernel/async/Socket.h>
#include <galay/kernel/async/TimerGenerator.h>
#include <galay/kernel/coroutine/AsyncWaiter.hpp>
#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "HttpParams.hpp" 

namespace galay::http
{
    class HttpsWriter
    { 
    public:
        HttpsWriter(AsyncSslSocket& socket, TimerGenerator& generator, const HttpSettings& params);

        AsyncResult<std::expected<void, HttpError>> 
            send(   HttpRequest& request, 
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));
        
        AsyncResult<std::expected<void, HttpError>> 
            sendChunkHeader(HttpRequestHeader& header,
                            std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));
        

         AsyncResult<std::expected<void, HttpError>> 
            sendChunkData(  std::string_view chunk,
                            bool is_last,
                            std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));

        AsyncResult<std::expected<void, HttpError>> 
            reply(  HttpResponse& response,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));
        
        AsyncResult<std::expected<void, HttpError>> 
            replyChunkHeader(   HttpResponseHeader& header,
                                std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));
        
        
         AsyncResult<std::expected<void, HttpError>> 
            replyChunkData(  std::string_view chunk,
                            bool is_last,
                            std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));

        // WebSocket 升级
        AsyncResult<std::expected<void, HttpError>>
            upgradeToWebSocket(HttpRequest& request,
                             std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));
        
        // HTTP/2 升级
        AsyncResult<std::expected<void, HttpError>>
            upgradeToHttp2(HttpRequest& request,
                         std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));
       
#ifdef __linux__
        // 使用 sendfile 发送文件（零拷贝，仅 Linux）
        AsyncResult<std::expected<long, HttpError>>
            sendfile(int file_fd, off_t offset, size_t length);
#endif
                            
    private:
        Coroutine<nil> sendData(    std::string data,
                                    std::shared_ptr<AsyncWaiter<void, HttpError>> waiter,
                                    std::chrono::milliseconds timeout);
                                    
        Coroutine<nil> sendChunkData(   std::string_view chunk,
                                        std::shared_ptr<AsyncWaiter<void, HttpError>> waiter,
                                        bool is_last,
                                        std::chrono::milliseconds timeout);

#ifdef __linux__
        Coroutine<nil> sendfileInternal(int file_fd, 
                                        off_t offset, 
                                        size_t length,
                                        std::shared_ptr<AsyncWaiter<long, HttpError>> waiter);
#endif

    private:
        AsyncSslSocket& m_socket;
        HttpSettings      m_params;
        TimerGenerator& m_generator;
    };
}

#endif