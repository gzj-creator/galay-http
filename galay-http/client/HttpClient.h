#ifndef GALAY_HTTP_CLIENT_H
#define GALAY_HTTP_CLIENT_H

#include <galay/kernel/async/Socket.h>
#include "galay-http/kernel/HttpParams.hpp"
#include "galay-http/kernel/HttpReader.h"
#include "galay-http/kernel/HttpWriter.h"
#include "galay/kernel/runtime/Runtime.h"

namespace galay::http
{
    class HttpClient 
    {
    public:
        HttpClient(Runtime& runtime, HttpSettings m_params = {});

        std::expected<void, CommonError> init();
        std::expected<void, CommonError> init(const Host& host);

        AsyncResult<std::expected<void, CommonError>> connect(const Host& host);

        HttpReader getReader();
        HttpWriter getWriter();
    private:
        AsyncTcpSocket m_socket;
        TimerGenerator m_generator;
        HttpSettings m_params;
    };
}

#endif