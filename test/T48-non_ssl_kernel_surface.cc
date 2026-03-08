#include <type_traits>

#include "galay-http/kernel/http/HttpConn.h"
#include "galay-http/kernel/http/HttpReader.h"
#include "galay-http/kernel/http/HttpRouter.h"
#include "galay-http/kernel/http/HttpSession.h"
#include "galay-http/kernel/http/HttpWriter.h"
#include "galay-http/kernel/http2/H2Client.h"
#include "galay-http/kernel/http2/H2cClient.h"
#include "galay-http/kernel/http2/Http2Conn.h"
#include "galay-http/kernel/websocket/WsConn.h"

int main() {
    static_assert(std::is_move_constructible_v<galay::http::HttpConn>);
    static_assert(std::is_move_constructible_v<galay::websocket::WsConn>);
    static_assert(std::is_move_constructible_v<galay::http2::H2cClient>);
    return 0;
}
