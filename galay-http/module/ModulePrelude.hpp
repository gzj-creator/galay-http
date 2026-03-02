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
#if __has_include(<galay-utils/algorithm/Base64.hpp>)
#include <galay-utils/algorithm/Base64.hpp>
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
#if __has_include("galay-http/kernel/SslRecvCompatAwaitable.h")
#include "galay-http/kernel/SslRecvCompatAwaitable.h"
#endif
#if __has_include("galay-http/kernel/http/HttpClient.h")
#include "galay-http/kernel/http/HttpClient.h"
#endif
#if __has_include("galay-http/kernel/http/HttpConn.h")
#include "galay-http/kernel/http/HttpConn.h"
#endif
#if __has_include("galay-http/kernel/http/HttpLog.h")
#include "galay-http/kernel/http/HttpLog.h"
#endif
#if __has_include("galay-http/kernel/http/HttpReader.h")
#include "galay-http/kernel/http/HttpReader.h"
#endif
#if __has_include("galay-http/kernel/http/HttpRouter.h")
#include "galay-http/kernel/http/HttpRouter.h"
#endif
#if __has_include("galay-http/kernel/http/HttpServer.h")
#include "galay-http/kernel/http/HttpServer.h"
#endif
#if __has_include("galay-http/kernel/http/HttpSession.h")
#include "galay-http/kernel/http/HttpSession.h"
#endif
#if __has_include("galay-http/kernel/http/HttpWriter.h")
#include "galay-http/kernel/http/HttpWriter.h"
#endif
#if __has_include("galay-http/kernel/http2/H2Client.h")
#include "galay-http/kernel/http2/H2Client.h"
#endif
#if __has_include("galay-http/kernel/http2/H2cClient.h")
#include "galay-http/kernel/http2/H2cClient.h"
#endif
#if __has_include("galay-http/kernel/http2/Http2Conn.h")
#include "galay-http/kernel/http2/Http2Conn.h"
#endif
#if __has_include("galay-http/kernel/http2/Http2Server.h")
#include "galay-http/kernel/http2/Http2Server.h"
#endif
#if __has_include("galay-http/kernel/http2/Http2Stream.h")
#include "galay-http/kernel/http2/Http2Stream.h"
#endif
#if __has_include("galay-http/kernel/http2/Http2StreamManager.h")
#include "galay-http/kernel/http2/Http2StreamManager.h"
#endif
#if __has_include("galay-http/kernel/websocket/WsClient.h")
#include "galay-http/kernel/websocket/WsClient.h"
#endif
#if __has_include("galay-http/kernel/websocket/WsConn.h")
#include "galay-http/kernel/websocket/WsConn.h"
#endif
#if __has_include("galay-http/kernel/websocket/WsHeartbeat.h")
#include "galay-http/kernel/websocket/WsHeartbeat.h"
#endif
#if __has_include("galay-http/kernel/websocket/WsReader.h")
#include "galay-http/kernel/websocket/WsReader.h"
#endif
#if __has_include("galay-http/kernel/websocket/WsReaderSetting.h")
#include "galay-http/kernel/websocket/WsReaderSetting.h"
#endif
#if __has_include("galay-http/kernel/websocket/WsSession.h")
#include "galay-http/kernel/websocket/WsSession.h"
#endif
#if __has_include("galay-http/kernel/websocket/WsUpgrade.h")
#include "galay-http/kernel/websocket/WsUpgrade.h"
#endif
#if __has_include("galay-http/kernel/websocket/WsWriter.h")
#include "galay-http/kernel/websocket/WsWriter.h"
#endif
#if __has_include("galay-http/kernel/websocket/WsWriterSetting.h")
#include "galay-http/kernel/websocket/WsWriterSetting.h"
#endif
#if __has_include("galay-http/module/ModulePrelude.hpp")
#include "galay-http/module/ModulePrelude.hpp"
#endif
#if __has_include("galay-http/protoc/http/HttpBase.h")
#include "galay-http/protoc/http/HttpBase.h"
#endif
#if __has_include("galay-http/protoc/http/HttpBody.h")
#include "galay-http/protoc/http/HttpBody.h"
#endif
#if __has_include("galay-http/protoc/http/HttpChunk.h")
#include "galay-http/protoc/http/HttpChunk.h"
#endif
#if __has_include("galay-http/protoc/http/HttpError.h")
#include "galay-http/protoc/http/HttpError.h"
#endif
#if __has_include("galay-http/protoc/http/HttpHeader.h")
#include "galay-http/protoc/http/HttpHeader.h"
#endif
#if __has_include("galay-http/protoc/http/HttpRequest.h")
#include "galay-http/protoc/http/HttpRequest.h"
#endif
#if __has_include("galay-http/protoc/http/HttpResponse.h")
#include "galay-http/protoc/http/HttpResponse.h"
#endif
#if __has_include("galay-http/protoc/http2/Http2Base.h")
#include "galay-http/protoc/http2/Http2Base.h"
#endif
#if __has_include("galay-http/protoc/http2/Http2Error.h")
#include "galay-http/protoc/http2/Http2Error.h"
#endif
#if __has_include("galay-http/protoc/http2/Http2Frame.h")
#include "galay-http/protoc/http2/Http2Frame.h"
#endif
#if __has_include("galay-http/protoc/http2/Http2Hpack.h")
#include "galay-http/protoc/http2/Http2Hpack.h"
#endif
#if __has_include("galay-http/protoc/websocket/WebSocketError.h")
#include "galay-http/protoc/websocket/WebSocketError.h"
#endif
#if __has_include("galay-http/protoc/websocket/WebSocketFrame.h")
#include "galay-http/protoc/websocket/WebSocketFrame.h"
#endif
#if __has_include("galay-http/protoc/websocket/WsBase.h")
#include "galay-http/protoc/websocket/WsBase.h"
#endif
#if __has_include("galay-http/utils/Http1_1RequestBuilder.h")
#include "galay-http/utils/Http1_1RequestBuilder.h"
#endif
#if __has_include("galay-http/utils/Http1_1ResponseBuilder.h")
#include "galay-http/utils/Http1_1ResponseBuilder.h"
#endif
#if __has_include("galay-http/utils/HttpLogger.h")
#include "galay-http/utils/HttpLogger.h"
#endif
#if __has_include("galay-http/utils/HttpUtils.h")
#include "galay-http/utils/HttpUtils.h"
#endif
#if __has_include("galay-kernel/async/TcpSocket.h")
#include "galay-kernel/async/TcpSocket.h"
#endif
#if __has_include("galay-kernel/common/Buffer.h")
#include "galay-kernel/common/Buffer.h"
#endif
#if __has_include("galay-kernel/common/Error.h")
#include "galay-kernel/common/Error.h"
#endif
#if __has_include("galay-kernel/common/Sleep.hpp")
#include "galay-kernel/common/Sleep.hpp"
#endif
#if __has_include("galay-kernel/concurrency/AsyncWaiter.h")
#include "galay-kernel/concurrency/AsyncWaiter.h"
#endif
#if __has_include("galay-kernel/concurrency/UnsafeChannel.h")
#include "galay-kernel/concurrency/UnsafeChannel.h"
#endif
#if __has_include("galay-kernel/kernel/Awaitable.h")
#include "galay-kernel/kernel/Awaitable.h"
#endif
#if __has_include("galay-kernel/kernel/Coroutine.h")
#include "galay-kernel/kernel/Coroutine.h"
#endif
#if __has_include("galay-kernel/kernel/IOHandlers.hpp")
#include "galay-kernel/kernel/IOHandlers.hpp"
#endif
#if __has_include("galay-kernel/kernel/Runtime.h")
#include "galay-kernel/kernel/Runtime.h"
#endif
#if __has_include("galay-kernel/kernel/Timeout.hpp")
#include "galay-kernel/kernel/Timeout.hpp"
#endif
#if __has_include("galay-ssl/async/SslSocket.h")
#include "galay-ssl/async/SslSocket.h"
#endif
#if __has_include("galay-ssl/ssl/SslContext.h")
#include "galay-ssl/ssl/SslContext.h"
#endif
