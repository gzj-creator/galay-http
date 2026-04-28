#pragma once
// Auto prelude for transitional C++23 module builds on Clang/GCC/MSVC.
// Keep third-party/system/dependency headers in global module fragment.

#if __has_include(<algorithm>)
#include <algorithm>
#endif
#if __has_include(<arm_neon.h>)
#include <arm_neon.h>
#endif
#if __has_include(<arpa/inet.h>)
#include <arpa/inet.h>
#endif
#if __has_include(<array>)
#include <array>
#endif
#if __has_include(<atomic>)
#include <atomic>
#endif
#if __has_include(<cassert>)
#include <cassert>
#endif
#if __has_include(<cctype>)
#include <cctype>
#endif
#if __has_include(<chrono>)
#include <chrono>
#endif
#if __has_include(<concepts>)
#include <concepts>
#endif
#if __has_include(<coroutine>)
#include <coroutine>
#endif
#if __has_include(<cstddef>)
#include <cstddef>
#endif
#if __has_include(<cstdint>)
#include <cstdint>
#endif
#if __has_include(<cstring>)
#include <cstring>
#endif
#if __has_include(<ctime>)
#include <ctime>
#endif
#if __has_include(<expected>)
#include <expected>
#endif
#if __has_include(<fcntl.h>)
#include <fcntl.h>
#endif
#if __has_include(<filesystem>)
#include <filesystem>
#endif
#if __has_include(<fstream>)
#include <fstream>
#endif
#if __has_include(<functional>)
#include <functional>
#endif
#if __has_include(<galay-utils/algorithm/base64.hpp>)
#include <galay-utils/algorithm/base64.hpp>
#endif
#if __has_include(<iomanip>)
#include <iomanip>
#endif
#if __has_include(<locale>)
#include <locale>
#endif
#if __has_include(<map>)
#include <map>
#endif
#if __has_include(<memory>)
#include <memory>
#endif
#if __has_include(<openssl/bio.h>)
#include <openssl/bio.h>
#endif
#if __has_include(<openssl/buffer.h>)
#include <openssl/buffer.h>
#endif
#if __has_include(<openssl/evp.h>)
#include <openssl/evp.h>
#endif
#if __has_include(<openssl/sha.h>)
#include <openssl/sha.h>
#endif
#if __has_include(<optional>)
#include <optional>
#endif
#if __has_include(<queue>)
#include <queue>
#endif
#if __has_include(<random>)
#include <random>
#endif
#if __has_include(<regex>)
#include <regex>
#endif
#if __has_include(<set>)
#include <set>
#endif
#if __has_include(<spdlog/async.h>)
#include <spdlog/async.h>
#endif
#if __has_include(<spdlog/async_logger.h>)
#include <spdlog/async_logger.h>
#endif
#if __has_include(<spdlog/sinks/basic_file_sink.h>)
#include <spdlog/sinks/basic_file_sink.h>
#endif
#if __has_include(<spdlog/sinks/stdout_color_sinks.h>)
#include <spdlog/sinks/stdout_color_sinks.h>
#endif
#if __has_include(<spdlog/spdlog.h>)
#include <spdlog/spdlog.h>
#endif
#if __has_include(<sstream>)
#include <sstream>
#endif
#if __has_include(<string>)
#include <string>
#endif
#if __has_include(<string_view>)
#include <string_view>
#endif
#if __has_include(<sys/stat.h>)
#include <sys/stat.h>
#endif
#if __has_include(<sys/uio.h>)
#include <sys/uio.h>
#endif
#if __has_include(<system_error>)
#include <system_error>
#endif
#if __has_include(<time.h>)
#include <time.h>
#endif
#if __has_include(<type_traits>)
#include <type_traits>
#endif
#if __has_include(<unistd.h>)
#include <unistd.h>
#endif
#if __has_include(<unordered_map>)
#include <unordered_map>
#endif
#if __has_include(<utility>)
#include <utility>
#endif
#if __has_include(<variant>)
#include <variant>
#endif
#if __has_include(<vector>)
#include <vector>
#endif
#if __has_include("galay-http/kernel/http/http_client.h")
#include "galay-http/kernel/http/http_client.h"
#endif
#if __has_include("galay-http/kernel/http/http_conn.h")
#include "galay-http/kernel/http/http_conn.h"
#endif
#if __has_include("galay-http/kernel/http/http_log.h")
#include "galay-http/kernel/http/http_log.h"
#endif
#if __has_include("galay-http/kernel/http/http_reader.h")
#include "galay-http/kernel/http/http_reader.h"
#endif
#if __has_include("galay-http/kernel/http/http_router.h")
#include "galay-http/kernel/http/http_router.h"
#endif
#if __has_include("galay-http/kernel/http/http_server.h")
#include "galay-http/kernel/http/http_server.h"
#endif
#if __has_include("galay-http/kernel/http/http_session.h")
#include "galay-http/kernel/http/http_session.h"
#endif
#if __has_include("galay-http/kernel/http/http_writer.h")
#include "galay-http/kernel/http/http_writer.h"
#endif
#if __has_include("galay-http/kernel/http2/h2_client.h")
#include "galay-http/kernel/http2/h2_client.h"
#endif
#if __has_include("galay-http/kernel/http2/h2c_client.h")
#include "galay-http/kernel/http2/h2c_client.h"
#endif
#if __has_include("galay-http/kernel/http2/http2_conn.h")
#include "galay-http/kernel/http2/http2_conn.h"
#endif
#if __has_include("galay-http/kernel/http2/http2_server.h")
#include "galay-http/kernel/http2/http2_server.h"
#endif
#if __has_include("galay-http/kernel/http2/http2_stream.h")
#include "galay-http/kernel/http2/http2_stream.h"
#endif
#if __has_include("galay-http/kernel/http2/stream_mgr.h")
#include "galay-http/kernel/http2/stream_mgr.h"
#endif
#if __has_include("galay-http/kernel/websocket/ws_client.h")
#include "galay-http/kernel/websocket/ws_client.h"
#endif
#if __has_include("galay-http/kernel/websocket/ws_conn.h")
#include "galay-http/kernel/websocket/ws_conn.h"
#endif
#if __has_include("galay-http/kernel/websocket/ws_heartbeat.h")
#include "galay-http/kernel/websocket/ws_heartbeat.h"
#endif
#if __has_include("galay-http/kernel/websocket/ws_reader.h")
#include "galay-http/kernel/websocket/ws_reader.h"
#endif
#if __has_include("galay-http/kernel/websocket/reader_cfg.h")
#include "galay-http/kernel/websocket/reader_cfg.h"
#endif
#if __has_include("galay-http/kernel/websocket/ws_session.h")
#include "galay-http/kernel/websocket/ws_session.h"
#endif
#if __has_include("galay-http/kernel/websocket/ws_upgrade.h")
#include "galay-http/kernel/websocket/ws_upgrade.h"
#endif
#if __has_include("galay-http/kernel/websocket/ws_writer.h")
#include "galay-http/kernel/websocket/ws_writer.h"
#endif
#if __has_include("galay-http/kernel/websocket/writer_cfg.h")
#include "galay-http/kernel/websocket/writer_cfg.h"
#endif
#if __has_include("galay-http/module/module_prelude.hpp")
#include "galay-http/module/module_prelude.hpp"
#endif
#if __has_include("galay-http/protoc/http/http_base.h")
#include "galay-http/protoc/http/http_base.h"
#endif
#if __has_include("galay-http/protoc/http/http_body.h")
#include "galay-http/protoc/http/http_body.h"
#endif
#if __has_include("galay-http/protoc/http/http_chunk.h")
#include "galay-http/protoc/http/http_chunk.h"
#endif
#if __has_include("galay-http/protoc/http/http_error.h")
#include "galay-http/protoc/http/http_error.h"
#endif
#if __has_include("galay-http/protoc/http/http_header.h")
#include "galay-http/protoc/http/http_header.h"
#endif
#if __has_include("galay-http/protoc/http/http_request.h")
#include "galay-http/protoc/http/http_request.h"
#endif
#if __has_include("galay-http/protoc/http/http_response.h")
#include "galay-http/protoc/http/http_response.h"
#endif
#if __has_include("galay-http/protoc/http2/http2_base.h")
#include "galay-http/protoc/http2/http2_base.h"
#endif
#if __has_include("galay-http/protoc/http2/http2_error.h")
#include "galay-http/protoc/http2/http2_error.h"
#endif
#if __has_include("galay-http/protoc/http2/http2_frame.h")
#include "galay-http/protoc/http2/http2_frame.h"
#endif
#if __has_include("galay-http/protoc/http2/http2_hpack.h")
#include "galay-http/protoc/http2/http2_hpack.h"
#endif
#if __has_include("galay-http/protoc/websocket/ws_error.h")
#include "galay-http/protoc/websocket/ws_error.h"
#endif
#if __has_include("galay-http/protoc/websocket/ws_frame.h")
#include "galay-http/protoc/websocket/ws_frame.h"
#endif
#if __has_include("galay-http/protoc/websocket/ws_base.h")
#include "galay-http/protoc/websocket/ws_base.h"
#endif
#if __has_include("galay-http/utils/req_bld.h")
#include "galay-http/utils/req_bld.h"
#endif
#if __has_include("galay-http/utils/rsp_bld.h")
#include "galay-http/utils/rsp_bld.h"
#endif
#if __has_include("galay-http/utils/http_logger.h")
#include "galay-http/utils/http_logger.h"
#endif
#if __has_include("galay-http/utils/http_utils.h")
#include "galay-http/utils/http_utils.h"
#endif
#if __has_include("galay-kernel/async/tcp_socket.h")
#include "galay-kernel/async/tcp_socket.h"
#endif
#if __has_include("galay-kernel/common/buffer.h")
#include "galay-kernel/common/buffer.h"
#endif
#if __has_include("galay-kernel/common/error.h")
#include "galay-kernel/common/error.h"
#endif
#if __has_include("galay-kernel/common/sleep.hpp")
#include "galay-kernel/common/sleep.hpp"
#endif
#if __has_include("galay-kernel/concurrency/async_waiter.h")
#include "galay-kernel/concurrency/async_waiter.h"
#endif
#if __has_include("galay-kernel/concurrency/unsafe_channel.h")
#include "galay-kernel/concurrency/unsafe_channel.h"
#endif
#if __has_include("galay-kernel/kernel/awaitable.h")
#include "galay-kernel/kernel/awaitable.h"
#endif
#if __has_include("galay-kernel/kernel/task.h")
#include "galay-kernel/kernel/task.h"
#endif
#if __has_include("galay-kernel/kernel/io_handlers.hpp")
#include "galay-kernel/kernel/io_handlers.hpp"
#endif
#if __has_include("galay-kernel/kernel/runtime.h")
#include "galay-kernel/kernel/runtime.h"
#endif
#if __has_include("galay-kernel/kernel/timeout.hpp")
#include "galay-kernel/kernel/timeout.hpp"
#endif
#if __has_include("galay-ssl/async/ssl_socket.h")
#include "galay-ssl/async/ssl_socket.h"
#endif
#if __has_include("galay-ssl/ssl/ssl_context.h")
#include "galay-ssl/ssl/ssl_context.h"
#endif
