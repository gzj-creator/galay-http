#ifndef GALAY_HTTP_PARAMS_H
#define GALAY_HTTP_PARAMS_H 

#include "galay-http/protoc/HttpBase.h"

namespace galay::http
{
    struct HttpSettings {
        std::chrono::milliseconds recv_timeout = std::chrono::milliseconds(-1);
        std::chrono::milliseconds send_timeout = std::chrono::milliseconds(-1);

        size_t recv_incr_length     = DEFAULT_HTTP_PEER_STEP_SIZE;
        size_t max_header_size      = DEFAULT_HTTP_MAX_HEADER_SIZE;
        size_t chunk_buffer_size    = DEFAULT_HTTP_CHUNK_BUFFER_SIZE;
    };

}

#endif