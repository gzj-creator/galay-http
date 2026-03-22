#include "galay-http/kernel/http2/H2cClient.h"
#include "galay-http/kernel/http2/Http2StreamManager.h"
#include "galay-kernel/async/TcpSocket.h"
#include <type_traits>
#include <utility>

using namespace galay::http2;
using galay::async::TcpSocket;

static_assert(std::is_same_v<
              std::remove_cvref_t<decltype(std::declval<H2cClient&>().upgrade("/"))>,
              H2cUpgradeAwaitable>,
              "H2cClient::upgrade() should return the direct custom awaitable");

using H2cManager = Http2StreamManagerImpl<TcpSocket>;

static_assert(std::is_same_v<
              std::remove_cvref_t<decltype(std::declval<H2cManager&>().shutdown())>,
              galay::kernel::Task<void>>,
              "Http2StreamManager::shutdown() should return Task<void>");

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-http/kernel/http2/H2Client.h"

static_assert(std::is_same_v<
              std::remove_cvref_t<decltype(std::declval<H2Client&>().connect("localhost", 443))>,
              H2Client::ConnectAwaitable>,
              "H2Client::connect() should return the direct connect awaitable");
#endif

int main() {
    return 0;
}
