/**
 * @file http_reader.h
 * @brief HTTP 读取器，基于异步状态机从 Socket 读取并解析 HTTP 消息
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供 HttpReaderImpl 模板类，支持从 TcpSocket 或 SslSocket 读取
 * HTTP 请求、HTTP 响应和 HTTP Chunk 数据。
 * 内部使用 RingBuffer + iovec 零拷贝技术，结合异步状态机实现高效的
 * 非阻塞读取。支持明文 TCP（readv）和 SSL 两种 IO 模式。
 */

#ifndef GALAY_HTTP_READER_H
#define GALAY_HTTP_READER_H

#include "reader_cfg.h"
#include "galay-http/kernel/iov_utils.h"
#include "galay-http/protoc/http/http_chunk.h"
#include "galay-http/protoc/http/http_error.h"
#include "galay-http/protoc/http/http_request.h"
#include "galay-http/protoc/http/http_response.h"
#include "galay-kernel/async/tcp_socket.h"
#include "galay-kernel/common/buffer.h"
#include "galay-kernel/kernel/awaitable.h"
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/async/ssl_await.h"
#include "galay-ssl/async/ssl_socket.h"
#endif

namespace galay::http {

using namespace galay::async;
using namespace galay::kernel;

/**
 * @brief SSL Socket 类型判断特征（默认为 false）
 * @tparam T 待判断的类型
 */
template<typename T>
struct is_ssl_socket : std::false_type {};

#ifdef GALAY_HTTP_SSL_ENABLED
/**
 * @brief SslSocket 的特化，标记为 true
 */
template<>
struct is_ssl_socket<galay::ssl::SslSocket> : std::true_type {};
#endif

/**
 * @brief 判断 T 是否为 SSL Socket 的内联常量
 * @tparam T 待判断的类型
 */
template<typename T>
inline constexpr bool is_ssl_socket_v = is_ssl_socket<T>::value;

namespace detail {

/**
 * @brief HTTP RingBuffer TCP 读取状态机
 * @tparam StateT 读取状态类型（如 HttpRequestReadState）
 * @details 从 RingBuffer 读取数据并委托给 StateT 进行解析，
 *          使用 readv 系统调用实现零拷贝接收。
 */
template<typename StateT>
struct HttpRingBufferTcpReadMachine {
    using result_type = typename StateT::ResultType;
    static constexpr auto kSequenceOwnerDomain = galay::kernel::SequenceOwnerDomain::Read;

    explicit HttpRingBufferTcpReadMachine(std::shared_ptr<StateT> state)
        : m_state(std::move(state)) {}

    MachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        if (m_state->parseFromRingBuffer()) {
            m_result = m_state->takeResult();
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        if (!m_state->prepareRecvWindow()) {
            m_result = m_state->takeResult();
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        return MachineAction<result_type>::waitReadv(
            m_state->recvIovecsData(),
            m_state->recvIovecsCount());
    }

    void onRead(std::expected<size_t, IOError> result) {
        if (!result) {
            m_state->setRecvError(result.error());
            m_result = m_state->takeResult();
            return;
        }

        if (result.value() == 0) {
            m_state->onPeerClosed();
            m_result = m_state->takeResult();
            return;
        }

        m_state->onBytesReceived(result.value());
    }

    void onWrite(std::expected<size_t, IOError>) {}

    std::shared_ptr<StateT> m_state;
    std::optional<result_type> m_result;
};

#ifdef GALAY_HTTP_SSL_ENABLED
/**
 * @brief HTTP RingBuffer SSL 读取状态机
 * @tparam StateT 读取状态类型
 * @details SSL 版本的读取状态机，使用 SSL recv 接收数据。
 */
template<typename StateT>
struct HttpRingBufferSslReadMachine {
    using result_type = typename StateT::ResultType;
    static constexpr auto kSequenceOwnerDomain = galay::kernel::SequenceOwnerDomain::Read;

    explicit HttpRingBufferSslReadMachine(std::shared_ptr<StateT> state)
        : m_state(std::move(state)) {}

    galay::ssl::SslMachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        if (m_state->parseFromRingBuffer()) {
            m_result = m_state->takeResult();
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        char* recv_buffer = nullptr;
        size_t recv_length = 0;
        if (!m_state->prepareRecvWindow(recv_buffer, recv_length)) {
            m_result = m_state->takeResult();
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        return galay::ssl::SslMachineAction<result_type>::recv(recv_buffer, recv_length);
    }

    void onHandshake(std::expected<void, galay::ssl::SslError>) {}

    void onRecv(std::expected<Bytes, galay::ssl::SslError> result) {
        if (!result) {
            m_state->setSslRecvError(result.error());
            m_result = m_state->takeResult();
            return;
        }

        const size_t recv_bytes = result.value().size();
        if (recv_bytes == 0) {
            m_state->onPeerClosed();
            m_result = m_state->takeResult();
            return;
        }

        m_state->onBytesReceived(recv_bytes);
    }

    void onSend(std::expected<size_t, galay::ssl::SslError>) {}

    void onShutdown(std::expected<void, galay::ssl::SslError>) {}

    std::shared_ptr<StateT> m_state;
    std::optional<result_type> m_result;
};
#endif

/**
 * @brief HTTP 请求读取状态
 * @details 管理 RingBuffer 中 HTTP 请求的读取与解析过程，
 *          跟踪接收字节数、解析进度和错误状态。
 */
struct HttpRequestReadState {
    using ResultType = std::expected<bool, HttpError>; ///< 结果类型

    /**
     * @brief 构造函数
     * @param ring_buffer 环形缓冲区引用
     * @param setting 读取器配置
     * @param request 待填充的 HTTP 请求对象
     */
    HttpRequestReadState(RingBuffer& ring_buffer,
                         const HttpReaderSetting& setting,
                         HttpRequest& request)
        : m_ring_buffer(&ring_buffer)
        , m_setting(&setting)
        , m_request(&request) {}

    /**
     * @brief 重置状态用于下一次读取
     * @param request 新的 HTTP 请求对象
     */
    void resetForNextRead(HttpRequest& request) {
        m_request = &request;
        m_request->reset();
        m_total_received = 0;
        m_parse_iovecs.clear();
        m_write_iovecs = {};
        m_http_error.reset();
    }

    /**
     * @brief 从 RingBuffer 中尝试解析 HTTP 请求
     * @return 解析完成返回 true，数据不足返回 false，出错时也返回 true（通过 takeResult 获取错误）
     */
    bool parseFromRingBuffer() {
        auto read_iovecs = borrowReadIovecs(*m_ring_buffer);
        if (read_iovecs.empty()) {
            return false;
        }

        if (IoVecWindow::buildWindow(read_iovecs, m_parse_iovecs) == 0) {
            return false;
        }

        auto [error_code, consumed] = m_request->fromIOVec(m_parse_iovecs);
        if (consumed > 0) {
            m_ring_buffer->consume(consumed);
        }

        if (error_code == kHeaderInComplete || error_code == kIncomplete) {
            if (m_total_received >= m_setting->getMaxHeaderSize() && !m_request->isComplete()) {
                setParseError(HttpError(kHeaderTooLarge));
                return true;
            }
            return false;
        }

        if (error_code != kNoError) {
            setParseError(HttpError(error_code));
            return true;
        }

        if (!m_request->isComplete()) {
            return false;
        }

        auto& header = m_request->header();
        const std::string host = header.headerPairs().getValue("host");
        return true;
    }

    /**
     * @brief 准备接收窗口（用于 readv）
     * @return 成功返回 true，缓冲区满返回 false
     */
    bool prepareRecvWindow() {
        m_write_iovecs = borrowWriteIovecs(*m_ring_buffer);
        if (m_write_iovecs.empty()) {
            setParseError(HttpError(kHeaderTooLarge));
            return false;
        }
        return true;
    }

    bool prepareRecvWindow(char*& buffer, size_t& length) {
        if (!prepareRecvWindow()) {
            buffer = nullptr;
            length = 0;
            return false;
        }
        if (!IoVecWindow::bindFirstNonEmpty(m_write_iovecs, buffer, length)) {
            setParseError(HttpError(kHeaderTooLarge));
            return false;
        }
        return true;
    }

    const struct iovec* recvIovecsData() const { return m_write_iovecs.data(); }
    size_t recvIovecsCount() const { return m_write_iovecs.size(); }

    void setRecvError(const IOError& io_error) {
        if (IOError::contains(io_error.code(), kDisconnectError)) {
            m_http_error = HttpError(kConnectionClose);
            return;
        }
        m_http_error = HttpError(kRecvError, io_error.message());
    }

#ifdef GALAY_HTTP_SSL_ENABLED
    /**
     * @brief 设置 SSL 接收错误
     * @param error SSL 错误
     */
    void setSslRecvError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            m_http_error = HttpError(kConnectionClose);
            return;
        }
        m_http_error = HttpError(kRecvError, error.message());
    }
#endif

    void onPeerClosed() { m_http_error = HttpError(kConnectionClose); } ///< 对端关闭连接

    /**
     * @brief 处理接收到的字节数
     * @param recv_bytes 接收字节数
     */
    void onBytesReceived(size_t recv_bytes) {
        m_ring_buffer->produce(recv_bytes);
        m_total_received += recv_bytes;
    }

    void setParseError(HttpError&& error) { m_http_error = std::move(error); } ///< 设置解析错误

    /**
     * @brief 获取读取结果
     * @return 成功返回 true，失败返回 HttpError
     */
    ResultType takeResult() {
        if (m_http_error.has_value()) {
            return std::unexpected(std::move(*m_http_error));
        }
        return true;
    }

    RingBuffer* m_ring_buffer;                          ///< 环形缓冲区指针
    const HttpReaderSetting* m_setting;                 ///< 读取器配置指针
    HttpRequest* m_request;                             ///< HTTP 请求对象指针
    size_t m_total_received = 0;                        ///< 已接收总字节数
    std::vector<iovec> m_parse_iovecs;                  ///< 解析用 iovec 缓冲
    BorrowedIovecs<2> m_write_iovecs;                   ///< 接收窗口 iovec
    std::optional<HttpError> m_http_error;              ///< HTTP 解析错误
};

/**
 * @brief HTTP 响应读取状态
 * @details 管理 RingBuffer 中 HTTP 响应的读取与解析过程
 */
struct HttpResponseReadState {
    using ResultType = std::expected<bool, HttpError>; ///< 结果类型

    /**
     * @brief 构造函数
     * @param ring_buffer 环形缓冲区引用
     * @param setting 读取器配置
     * @param response 待填充的 HTTP 响应对象
     */
    HttpResponseReadState(RingBuffer& ring_buffer,
                          const HttpReaderSetting& setting,
                          HttpResponse& response)
        : m_ring_buffer(&ring_buffer)
        , m_setting(&setting)
        , m_response(&response) {}

    /**
     * @brief 从 RingBuffer 中尝试解析 HTTP 响应
     * @return 解析完成返回 true，数据不足返回 false
     */
    bool parseFromRingBuffer() {
        auto read_iovecs = borrowReadIovecs(*m_ring_buffer);
        if (read_iovecs.empty()) {
            return false;
        }

        if (IoVecWindow::buildWindow(read_iovecs, m_parse_iovecs) == 0) {
            return false;
        }

        auto [error_code, consumed] = m_response->fromIOVec(m_parse_iovecs);
        if (consumed > 0) {
            m_ring_buffer->consume(consumed);
        }

        if (error_code == kHeaderInComplete || error_code == kIncomplete) {
            if (m_total_received >= m_setting->getMaxHeaderSize() && !m_response->isComplete()) {
                setParseError(HttpError(kHeaderTooLarge));
                return true;
            }
            return false;
        }

        if (error_code != kNoError) {
            setParseError(HttpError(error_code));
            return true;
        }

        return m_response->isComplete();
    }

    /**
     * @brief 准备接收窗口
     * @return 成功返回 true
     */
    bool prepareRecvWindow() {
        m_write_iovecs = borrowWriteIovecs(*m_ring_buffer);
        if (m_write_iovecs.empty()) {
            setParseError(HttpError(kHeaderTooLarge));
            return false;
        }
        return true;
    }

    /**
     * @brief 准备 SSL 接收窗口
     * @param[out] buffer 输出缓冲区指针
     * @param[out] length 输出缓冲区长度
     * @return 成功返回 true
     */
    bool prepareRecvWindow(char*& buffer, size_t& length) {
        if (!prepareRecvWindow()) {
            buffer = nullptr;
            length = 0;
            return false;
        }
        if (!IoVecWindow::bindFirstNonEmpty(m_write_iovecs, buffer, length)) {
            setParseError(HttpError(kHeaderTooLarge));
            return false;
        }
        return true;
    }

    const struct iovec* recvIovecsData() const { return m_write_iovecs.data(); } ///< 获取接收 iovec 数据指针
    size_t recvIovecsCount() const { return m_write_iovecs.size(); } ///< 获取接收 iovec 数量

    /**
     * @brief 设置 TCP 接收错误
     * @param io_error IO 错误
     */
    void setRecvError(const IOError& io_error) {
        if (IOError::contains(io_error.code(), kDisconnectError)) {
            m_http_error = HttpError(kConnectionClose);
            return;
        }
        m_http_error = HttpError(kRecvError, io_error.message());
    }

#ifdef GALAY_HTTP_SSL_ENABLED
    /**
     * @brief 设置 SSL 接收错误
     * @param error SSL 错误
     */
    void setSslRecvError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            m_http_error = HttpError(kConnectionClose);
            return;
        }
        m_http_error = HttpError(kRecvError, error.message());
    }
#endif

    void onPeerClosed() { m_http_error = HttpError(kConnectionClose); } ///< 对端关闭连接

    /**
     * @brief 处理接收到的字节数
     * @param recv_bytes 接收字节数
     */
    void onBytesReceived(size_t recv_bytes) {
        m_ring_buffer->produce(recv_bytes);
        m_total_received += recv_bytes;
    }

    void setParseError(HttpError&& error) { m_http_error = std::move(error); } ///< 设置解析错误

    /**
     * @brief 获取读取结果
     * @return 成功返回 true，失败返回 HttpError
     */
    ResultType takeResult() {
        if (m_http_error.has_value()) {
            return std::unexpected(std::move(*m_http_error));
        }
        return true;
    }

    RingBuffer* m_ring_buffer;                          ///< 环形缓冲区指针
    const HttpReaderSetting* m_setting;                 ///< 读取器配置指针
    HttpResponse* m_response;                           ///< HTTP 响应对象指针
    size_t m_total_received = 0;                        ///< 已接收总字节数
    std::vector<iovec> m_parse_iovecs;                  ///< 解析用 iovec 缓冲
    BorrowedIovecs<2> m_write_iovecs;                   ///< 接收窗口 iovec
    std::optional<HttpError> m_http_error;              ///< HTTP 解析错误
};

/**
 * @brief HTTP Chunked 传输编码读取状态
 * @details 管理 HTTP chunked 编码数据的读取与解析
 */
struct HttpChunkReadState {
    using ResultType = std::expected<bool, HttpError>; ///< 结果类型

    /**
     * @brief 构造函数
     * @param ring_buffer 环形缓冲区引用
     * @param chunk_data 用于存储 chunk 数据的字符串引用
     */
    HttpChunkReadState(RingBuffer& ring_buffer, std::string& chunk_data)
        : m_ring_buffer(&ring_buffer)
        , m_chunk_data(&chunk_data) {}

    /**
     * @brief 从 RingBuffer 中尝试解析 chunked 数据
     * @return 最后一个 chunk 解析完成返回 true
     */
    bool parseFromRingBuffer() {
        auto read_iovecs = borrowReadIovecs(*m_ring_buffer);
        if (read_iovecs.empty()) {
            return false;
        }

        if (IoVecWindow::buildWindow(read_iovecs, m_parse_iovecs) == 0) {
            return false;
        }

        auto result = Chunk::fromIOVec(m_parse_iovecs, *m_chunk_data);
        if (!result) {
            const auto& error = result.error();
            if (error.code() == kIncomplete) {
                return false;
            }
            setParseError(HttpError(error.code(), error.message()));
            return true;
        }

        auto [is_last, consumed] = result.value();
        m_ring_buffer->consume(consumed);
        m_is_last = is_last;
        return is_last;
    }

    /**
     * @brief 准备接收窗口
     * @return 成功返回 true
     */
    bool prepareRecvWindow() {
        m_write_iovecs = borrowWriteIovecs(*m_ring_buffer);
        if (m_write_iovecs.empty()) {
            setParseError(HttpError(kRecvError, "RingBuffer is full"));
            return false;
        }
        return true;
    }

    /**
     * @brief 准备 SSL 接收窗口
     * @param[out] buffer 输出缓冲区指针
     * @param[out] length 输出缓冲区长度
     * @return 成功返回 true
     */
    bool prepareRecvWindow(char*& buffer, size_t& length) {
        if (!prepareRecvWindow()) {
            buffer = nullptr;
            length = 0;
            return false;
        }
        if (!IoVecWindow::bindFirstNonEmpty(m_write_iovecs, buffer, length)) {
            setParseError(HttpError(kRecvError, "RingBuffer is full"));
            return false;
        }
        return true;
    }

    const struct iovec* recvIovecsData() const { return m_write_iovecs.data(); } ///< 获取接收 iovec 数据指针
    size_t recvIovecsCount() const { return m_write_iovecs.size(); } ///< 获取接收 iovec 数量

    void setRecvError(const IOError& io_error) {
        if (IOError::contains(io_error.code(), kDisconnectError)) {
            m_http_error = HttpError(kConnectionClose);
            return;
        }
        m_http_error = HttpError(kRecvError, io_error.message());
    }

#ifdef GALAY_HTTP_SSL_ENABLED
    /**
     * @brief 设置 SSL 接收错误
     * @param error SSL 错误
     */
    void setSslRecvError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            m_http_error = HttpError(kConnectionClose);
            return;
        }
        m_http_error = HttpError(kRecvError, error.message());
    }
#endif

    void onPeerClosed() { m_http_error = HttpError(kConnectionClose); } ///< 对端关闭连接
    void onBytesReceived(size_t recv_bytes) { m_ring_buffer->produce(recv_bytes); } ///< 处理接收到的字节数
    void setParseError(HttpError&& error) { m_http_error = std::move(error); } ///< 设置解析错误

    /**
     * @brief 获取读取结果
     * @return 最后一个 chunk 返回 true，失败返回 HttpError
     */
    ResultType takeResult() {
        if (m_http_error.has_value()) {
            return std::unexpected(std::move(*m_http_error));
        }
        return m_is_last;
    }

    RingBuffer* m_ring_buffer;                          ///< 环形缓冲区指针
    std::string* m_chunk_data;                          ///< chunk 数据输出
    std::vector<iovec> m_parse_iovecs;                  ///< 解析用 iovec 缓冲
    BorrowedIovecs<2> m_write_iovecs;                   ///< 接收窗口 iovec
    std::optional<HttpError> m_http_error;              ///< HTTP 解析错误
    bool m_is_last = false;                             ///< 是否为最后一个 chunk
};

/**
 * @brief 构建异步读取操作
 * @tparam SocketType Socket 类型
 * @tparam StateT 读取状态类型
 * @param socket Socket 引用
 * @param state 共享的读取状态
 * @return 可 co_await 的异步操作对象
 */
template<typename SocketType, typename StateT>
auto buildReadOperation(SocketType& socket, std::shared_ptr<StateT> state) {
    using ResultType = typename StateT::ResultType;
    if constexpr (is_ssl_socket_v<SocketType>) {
#ifdef GALAY_HTTP_SSL_ENABLED
        return galay::ssl::SslAwaitableBuilder<ResultType>::fromStateMachine(
                   socket.controller(),
                   &socket,
                   HttpRingBufferSslReadMachine<StateT>(std::move(state)))
            .build();
#else
        static_assert(!sizeof(SocketType), "SSL support is disabled");
#endif
    } else {
        return AwaitableBuilder<ResultType>::fromStateMachine(
                   socket.controller(),
                   HttpRingBufferTcpReadMachine<StateT>(std::move(state)))
            .build();
    }
}

} // namespace detail

/**
 * @brief HTTP 读取器模板类
 * @tparam SocketType Socket 类型（TcpSocket 或 SslSocket）
 * @details 从 Socket 异步读取 HTTP 请求、响应和 chunked 数据。
 *          内部使用 RingBuffer + iovec 零拷贝技术和异步状态机。
 */
template<typename SocketType>
class HttpReaderImpl {
public:
    /**
     * @brief 构造函数
     * @param ring_buffer 环形缓冲区引用
     * @param setting 读取器配置
     * @param socket Socket 引用
     */
    HttpReaderImpl(RingBuffer& ring_buffer, const HttpReaderSetting& setting, SocketType& socket)
        : m_ring_buffer(&ring_buffer)
        , m_setting(setting)
        , m_socket(&socket) {}

    /**
     * @brief 异步读取一个完整的 HTTP 请求
     * @param request 待填充的 HTTP 请求对象
     * @return 可 co_await 的异步操作，成功返回 true，失败返回 HttpError
     */
    auto getRequest(HttpRequest& request) {
        auto state = getReusableRequestReadState(request);
        return detail::buildReadOperation(*m_socket, std::move(state));
    }

    /**
     * @brief 异步读取一个完整的 HTTP 响应
     * @param response 待填充的 HTTP 响应对象
     * @return 可 co_await 的异步操作，成功返回 true，失败返回 HttpError
     */
    auto getResponse(HttpResponse& response) {
        return detail::buildReadOperation(
            *m_socket,
            std::make_shared<detail::HttpResponseReadState>(*m_ring_buffer, m_setting, response));
    }

    /**
     * @brief 异步读取一个 HTTP chunked 编码的 chunk
     * @param chunk_data 用于存储 chunk 数据的字符串
     * @return 可 co_await 的异步操作，最后一个 chunk 返回 true，失败返回 HttpError
     */
    auto getChunk(std::string& chunk_data) {
        return detail::buildReadOperation(
            *m_socket,
            std::make_shared<detail::HttpChunkReadState>(*m_ring_buffer, chunk_data));
    }

private:
    /**
     * @brief 获取可复用的请求读取状态（减少内存分配）
     * @param request HTTP 请求对象
     * @return 共享的请求读取状态
     */
    std::shared_ptr<detail::HttpRequestReadState> getReusableRequestReadState(HttpRequest& request) {
        if (m_request_read_state && m_request_read_state.use_count() == 1) {
            m_request_read_state->resetForNextRead(request);
            return m_request_read_state;
        }

        m_request_read_state = std::make_shared<detail::HttpRequestReadState>(
            *m_ring_buffer,
            m_setting,
            request);
        return m_request_read_state;
    }

    RingBuffer* m_ring_buffer;                                          ///< 环形缓冲区指针
    HttpReaderSetting m_setting;                                        ///< 读取器配置
    SocketType* m_socket;                                               ///< Socket 指针
    std::shared_ptr<detail::HttpRequestReadState> m_request_read_state; ///< 可复用的请求读取状态
};

using HttpReader = HttpReaderImpl<TcpSocket>; ///< HTTP 明文读取器类型别名

} // namespace galay::http

#ifdef GALAY_HTTP_SSL_ENABLED
namespace galay::http {
using HttpsReader = HttpReaderImpl<galay::ssl::SslSocket>;
} // namespace galay::http
#endif

#endif // GALAY_HTTP_READER_H
