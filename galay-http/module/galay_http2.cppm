module;

#include "galay-http/module/module_prelude.hpp"

export module galay.http2;

export {
#include "galay-http/protoc/http2/http2_base.h"
#include "galay-http/protoc/http2/http2_error.h"
#include "galay-http/protoc/http2/http2_frame.h"
#include "galay-http/protoc/http2/http2_hpack.h"

#include "galay-http/kernel/http2/h2c_client.h"
#include "galay-http/kernel/http2/http2_conn.h"
#include "galay-http/kernel/http2/http2_server.h"
#include "galay-http/kernel/http2/http2_stream.h"
#include "galay-http/kernel/http2/stream_mgr.h"

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-http/kernel/http2/h2_client.h"
#endif
}
