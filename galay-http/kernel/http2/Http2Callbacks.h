#ifndef GALAY_HTTP2_CALLBACKS_H
#define GALAY_HTTP2_CALLBACKS_H

#include "galay-http/protoc/http2/Http2Frame.h"
#include "galay-http/protoc/http2/Http2Error.h"
#include "Http2Stream.h"
#include "Http2Header.h"
#include <functional>
#include <map>
#include <string>

namespace galay::http
{
    class Http2Stream;
    class Http2Header;
    
    using Http2StreamPtr = Http2Stream::ptr;
    
    /**
     * @brief HTTP/2 回调函数类型定义
     */
    using OnHeadersCallback = std::function<Coroutine<nil>(
        Http2StreamPtr stream,
        Http2Header headers,
        bool end_stream
    )>;
    
    using OnDataCallback = std::function<Coroutine<nil>(
        Http2StreamPtr stream,
        const std::string& data,
        bool end_stream
    )>;
    
    using OnSettingsCallback = std::function<Coroutine<nil>(
        const std::map<Http2SettingsId, uint32_t>& settings,
        bool is_ack
    )>;
    
    using OnPingCallback = std::function<Coroutine<nil>(
        uint64_t data,
        bool is_ack
    )>;
    
    using OnGoawayCallback = std::function<Coroutine<nil>(
        uint32_t last_stream_id,
        Http2ErrorCode error_code,
        const std::string& debug_data
    )>;
    
    using OnWindowUpdateCallback = std::function<Coroutine<nil>(
        Http2StreamPtr stream,
        uint32_t window_size_increment
    )>;
    
    using OnRstStreamCallback = std::function<Coroutine<nil>(
        Http2StreamPtr stream,
        Http2ErrorCode error_code
    )>;
    
    using OnPriorityCallback = std::function<Coroutine<nil>(
        Http2StreamPtr stream,
        uint32_t depends_on,
        uint8_t weight,
        bool exclusive
    )>;
    
    using OnErrorCallback = std::function<Coroutine<nil>(
        const Http2Error& error
    )>;
    
    struct Http2Callbacks
    {
        OnHeadersCallback on_headers = nullptr;
        OnDataCallback on_data = nullptr;
        
        OnSettingsCallback on_settings = nullptr;
        OnPingCallback on_ping = nullptr;
        OnGoawayCallback on_goaway = nullptr;
        OnWindowUpdateCallback on_window_update = nullptr;
        OnRstStreamCallback on_rst_stream = nullptr;
        OnPriorityCallback on_priority = nullptr;
        
        OnErrorCallback on_error = nullptr;
        
        bool isValid() const
        {
            return on_headers != nullptr && on_data != nullptr;
        }
        
        static Http2Callbacks createDefault();
    };
}

#endif // GALAY_HTTP2_CALLBACKS_H

