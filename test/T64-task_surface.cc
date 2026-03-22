#include <functional>
#include <type_traits>

#include "galay-http/kernel/http/HttpRouter.h"
#include "galay-http/kernel/http/HttpServer.h"
#include "galay-http/kernel/http2/Http2Server.h"
#include "galay-http/kernel/http2/Http2StreamManager.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/kernel/Task.h"

using galay::async::TcpSocket;
using galay::kernel::Task;

static_assert(std::is_same_v<
              galay::http::HttpRouteHandler,
              std::function<Task<void>(galay::http::HttpConn&, galay::http::HttpRequest)>>,
              "HttpRouteHandler must use Task<void>");

static_assert(std::is_same_v<
              galay::http::HttpConnHandlerImpl<TcpSocket>,
              std::function<Task<void>(galay::http::HttpConnImpl<TcpSocket>)>>,
              "HttpConnHandlerImpl must use Task<void>");

static_assert(std::is_same_v<
              galay::http2::Http2StreamHandler,
              std::function<Task<void>(galay::http2::Http2Stream::ptr)>>,
              "Http2StreamHandler must use Task<void>");

static_assert(std::is_same_v<
              galay::http2::Http2ActiveConnHandler,
              std::function<Task<void>(galay::http2::Http2ConnContext&)>>,
              "Http2ActiveConnHandler must use Task<void>");

static_assert(std::is_same_v<
              galay::http2::Http1FallbackHandler,
              std::function<Task<void>(galay::http::HttpConnImpl<TcpSocket>,
                                       galay::http::HttpRequestHeader)>>,
              "Http1FallbackHandler must use Task<void>");

int main() {
    return 0;
}
