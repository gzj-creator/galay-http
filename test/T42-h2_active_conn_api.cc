/**
 * @file T42-H2ActiveConnApi.cc
 * @brief Active connection HTTP/2 API contract
 */

#include "galay-http/kernel/http2/Http2Server.h"
#include <type_traits>
#include <iostream>

using namespace galay::http2;

template<typename T>
concept HasGetActiveStreams = requires(T& ctx) {
    ctx.getActiveStreams(64);
};

template<typename T>
concept HasTakeEvents = requires(T* stream) {
    stream->takeEvents();
};

template<typename T>
concept HasChunkedRequestBody = requires(T* stream) {
    { stream->request().bodySize() } -> std::same_as<size_t>;
    { stream->request().bodyChunkCount() } -> std::same_as<size_t>;
    { stream->request().takeBodyChunks() } -> std::same_as<std::vector<std::string>>;
    { stream->request().takeCoalescedBody() } -> std::same_as<std::string>;
};

int main() {
    static_assert(requires(H2cServerBuilder& builder) {
        builder.activeConnHandler(
            [](auto&) -> galay::kernel::Coroutine { co_return; }
        );
    }, "H2cServerBuilder must expose activeConnHandler(handler)");

#ifdef GALAY_HTTP_SSL_ENABLED
    static_assert(requires(H2ServerBuilder& builder) {
        builder.activeConnHandler(
            [](auto&) -> galay::kernel::Coroutine { co_return; }
        );
    }, "H2ServerBuilder must expose activeConnHandler(handler)");
#endif

    static_assert(HasGetActiveStreams<Http2ConnContext>,
                  "Http2ConnContext must expose getActiveStreams(max_count)");

    static_assert(HasTakeEvents<Http2Stream>,
                  "Http2Stream must expose takeEvents()");

    static_assert(HasChunkedRequestBody<Http2Stream>,
                  "Http2Stream request surface must expose chunked body APIs");

    std::cout << "T42-H2ActiveConnApi PASS\n";
    return 0;
}
