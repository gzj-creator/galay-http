#ifndef GALAY_WS_CONN_H
#define GALAY_WS_CONN_H

#include "WsReader.h"
#include "WsWriter.h"
#include "galay-http/kernel/http/HttpConn.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/common/Buffer.h"

namespace galay::websocket
{

using namespace galay::async;
using namespace galay::kernel;

template<typename SocketType>
class WsConnImpl;

namespace detail {

template<typename SocketType>
struct WsEchoMachine {
    using result_type = std::expected<bool, WsError>;
    static constexpr auto kSequenceOwnerDomain = galay::kernel::SequenceOwnerDomain::ReadWrite;

    static WsWriterSetting resolveWriterSetting(WsConnImpl<SocketType>* conn, WsWriterSetting setting) {
        setting.use_mask = !conn->m_is_server;
        return setting;
    }

    WsEchoMachine(WsConnImpl<SocketType>* conn,
                  const WsReaderSetting& reader_setting,
                  WsWriterSetting writer_setting,
                  std::string& message,
                  WsOpcode& opcode)
        : m_conn(conn)
        , m_reader_setting(reader_setting)
        , m_read_state(conn->m_ring_buffer,
                       m_reader_setting,
                       message,
                       opcode,
                       conn->m_is_server,
                       !conn->m_is_server,
                       nullptr)
        , m_writer(resolveWriterSetting(conn, writer_setting), conn->m_socket)
        , m_message(&message)
        , m_opcode(&opcode) {}

    MachineAction<result_type> advance() {
        ++m_conn->m_echo_counters.composite_advance_calls;
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        if (m_stage == Stage::kRead) {
            return advanceRead();
        }
        return advanceWrite();
    }

    void onRead(std::expected<size_t, IOError> result) {
        if (!result) {
            m_read_state.setRecvError(result.error());
            m_result = m_read_state.takeResult();
            return;
        }

        if (result.value() == 0) {
            m_read_state.onPeerClosed();
            m_result = m_read_state.takeResult();
            return;
        }

        m_read_state.onBytesReceived(result.value());
    }

    void onWrite(std::expected<size_t, IOError> result) {
        if (!result) {
            m_result = std::unexpected(WsError(kWsSendError, result.error().message()));
            return;
        }

        const size_t written = result.value();
        if (written > 0) {
            m_writer.updateRemainingWritev(written);
        }

        if (m_writer.getRemainingBytes() == 0) {
            m_result = true;
            return;
        }

        if (m_writer.getIovecsData() == nullptr || m_writer.getIovecsCount() == 0) {
            m_result = std::unexpected(WsError(kWsSendError, "No remaining iovec to write"));
        }
    }

private:
    enum class Stage : uint8_t {
        kRead,
        kWrite,
    };

    MachineAction<result_type> advanceRead() {
        if (m_read_state.parseFromBuffer()) {
            return onParsedMessage();
        }

        if (!m_read_state.prepareRecvWindow()) {
            m_result = m_read_state.takeResult();
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        return MachineAction<result_type>::waitReadv(
            m_read_state.recvIovecsData(),
            m_read_state.recvIovecsCount());
    }

    MachineAction<result_type> onParsedMessage() {
        auto parsed = m_read_state.takeResult();
        if (!parsed.has_value()) {
            m_result = std::unexpected(parsed.error());
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        if (*m_opcode == WsOpcode::Text) {
            ++m_conn->m_echo_counters.composite_hits;
            m_writer.prepareSendFrame(WsFrameParser::createTextFrame(*m_message));
            m_stage = Stage::kWrite;
            return advanceWrite();
        }

        if (*m_opcode == WsOpcode::Binary) {
            ++m_conn->m_echo_counters.composite_hits;
            m_writer.prepareSendFrame(WsFrameParser::createBinaryFrame(*m_message));
            m_stage = Stage::kWrite;
            return advanceWrite();
        }

        ++m_conn->m_echo_counters.composite_fallbacks;
        m_result = true;
        return MachineAction<result_type>::complete(true);
    }

    MachineAction<result_type> advanceWrite() {
        if (m_writer.getRemainingBytes() == 0) {
            m_result = true;
            return MachineAction<result_type>::complete(true);
        }

        const auto* iov_data = m_writer.getIovecsData();
        const auto iov_count = m_writer.getIovecsCount();
        if (iov_data == nullptr || iov_count == 0) {
            m_result = std::unexpected(WsError(kWsSendError, "No remaining iovec to write"));
            return MachineAction<result_type>::complete(std::move(*m_result));
        }
        return MachineAction<result_type>::waitWritev(iov_data, iov_count);
    }

    WsConnImpl<SocketType>* m_conn;
    WsReaderSetting m_reader_setting;
    WsMessageReadState m_read_state;
    WsWriterImpl<SocketType> m_writer;
    std::string* m_message;
    WsOpcode* m_opcode;
    Stage m_stage = Stage::kRead;
    std::optional<result_type> m_result;
};

#ifdef GALAY_HTTP_SSL_ENABLED
template<typename SocketType>
struct WsSslEchoMachine {
    using result_type = std::expected<bool, WsError>;
    static constexpr auto kSequenceOwnerDomain = galay::kernel::SequenceOwnerDomain::ReadWrite;

    static WsWriterSetting resolveWriterSetting(WsConnImpl<SocketType>* conn, WsWriterSetting setting) {
        setting.use_mask = !conn->m_is_server;
        return setting;
    }

    WsSslEchoMachine(WsConnImpl<SocketType>* conn,
                     const WsReaderSetting& reader_setting,
                     WsWriterSetting writer_setting,
                     std::string& message,
                     WsOpcode& opcode)
        : m_conn(conn)
        , m_reader_setting(reader_setting)
        , m_read_state(conn->m_ring_buffer,
                       m_reader_setting,
                       message,
                       opcode,
                       conn->m_is_server,
                       !conn->m_is_server,
                       nullptr)
        , m_writer(resolveWriterSetting(conn, writer_setting), conn->m_socket)
        , m_message(&message)
        , m_opcode(&opcode) {}

    galay::ssl::SslMachineAction<result_type> advance() {
        ++m_conn->m_echo_counters.composite_advance_calls;
        if (m_result.has_value()) {
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        if (m_stage == Stage::kRead) {
            return advanceRead();
        }
        return advanceWrite();
    }

    void onHandshake(std::expected<void, galay::ssl::SslError>) {}

    void onRecv(std::expected<Bytes, galay::ssl::SslError> result) {
        if (!result) {
            m_read_state.setSslRecvError(result.error());
            m_result = m_read_state.takeResult();
            return;
        }

        const size_t recv_bytes = result.value().size();
        if (recv_bytes == 0) {
            m_read_state.onPeerClosed();
            m_result = m_read_state.takeResult();
            return;
        }

        m_read_state.onBytesReceived(recv_bytes);
    }

    void onSend(std::expected<size_t, galay::ssl::SslError> result) {
        if (!result) {
            m_writer.resetPendingState();
            m_result = std::unexpected(WsError(kWsSendError, result.error().message()));
            return;
        }

        if (result.value() == 0) {
            m_writer.resetPendingState();
            m_result = std::unexpected(WsError(kWsSendError, "SSL send returned zero bytes"));
            return;
        }

        m_writer.updateRemaining(result.value());
        if (m_writer.getRemainingBytes() == 0) {
            m_result = true;
        }
    }

    void onShutdown(std::expected<void, galay::ssl::SslError>) {}

private:
    enum class Stage : uint8_t {
        kRead,
        kWrite,
    };

    galay::ssl::SslMachineAction<result_type> advanceRead() {
        if (m_read_state.parseFromBuffer()) {
            return onParsedMessage();
        }

        char* recv_buffer = nullptr;
        size_t recv_length = 0;
        if (!m_read_state.prepareRecvWindow(recv_buffer, recv_length)) {
            m_result = m_read_state.takeResult();
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        return galay::ssl::SslMachineAction<result_type>::recv(recv_buffer, recv_length);
    }

    galay::ssl::SslMachineAction<result_type> onParsedMessage() {
        auto parsed = m_read_state.takeResult();
        if (!parsed.has_value()) {
            m_result = std::unexpected(parsed.error());
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        if (*m_opcode == WsOpcode::Text) {
            ++m_conn->m_echo_counters.composite_hits;
            ++m_conn->m_echo_counters.ssl_direct_message_hits;
            m_writer.prepareSslMessage(WsOpcode::Text, *m_message);
            m_stage = Stage::kWrite;
            return advanceWrite();
        }

        if (*m_opcode == WsOpcode::Binary) {
            ++m_conn->m_echo_counters.composite_hits;
            ++m_conn->m_echo_counters.ssl_direct_message_hits;
            m_writer.prepareSslMessage(WsOpcode::Binary, *m_message);
            m_stage = Stage::kWrite;
            return advanceWrite();
        }

        ++m_conn->m_echo_counters.composite_fallbacks;
        m_result = true;
        return galay::ssl::SslMachineAction<result_type>::complete(true);
    }

    galay::ssl::SslMachineAction<result_type> advanceWrite() {
        if (m_writer.getRemainingBytes() == 0) {
            m_result = true;
            return galay::ssl::SslMachineAction<result_type>::complete(true);
        }

        return galay::ssl::SslMachineAction<result_type>::send(
            m_writer.bufferData() + m_writer.sentBytes(),
            m_writer.getRemainingBytes());
    }

    WsConnImpl<SocketType>* m_conn;
    WsReaderSetting m_reader_setting;
    WsMessageReadState m_read_state;
    WsWriterImpl<SocketType> m_writer;
    std::string* m_message;
    WsOpcode* m_opcode;
    Stage m_stage = Stage::kRead;
    std::optional<result_type> m_result;
};
#endif

} // namespace detail

/**
 * @brief WebSocket连接模板类
 * @tparam SocketType Socket类型（TcpSocket 或 SslSocket）
 * @details 封装WebSocket连接的底层资源，不持有reader/writer，通过接口构造返回
 */
template<typename SocketType>
class WsConnImpl
{
public:
    struct EchoCounters {
        size_t composite_awaitables_started = 0;
        size_t composite_hits = 0;
        size_t composite_fallbacks = 0;
        size_t composite_advance_calls = 0;
        size_t ssl_direct_message_hits = 0;
    };

    /**
     * @brief 从HttpConn构造（用于升级场景）
     * @note 升级之后HttpConn不再可用
     */
    static WsConnImpl<SocketType> from(galay::http::HttpConnImpl<SocketType>&& http_conn, bool is_server = true)
    {
        return WsConnImpl<SocketType>(std::move(http_conn.m_socket), std::move(http_conn.m_ring_buffer), is_server);
    }

    /**
     * @brief 直接构造
     */
    WsConnImpl(SocketType&& socket, RingBuffer&& ring_buffer, bool is_server = true)
        : m_socket(std::move(socket))
        , m_ring_buffer(std::move(ring_buffer))
        , m_is_server(is_server)
    {
    }

    /**
     * @brief 构造函数（只持有socket）
     */
    WsConnImpl(SocketType&& socket, bool is_server = true)
        : m_socket(std::move(socket))
        , m_ring_buffer(8192)  // 默认8KB buffer
        , m_is_server(is_server)
    {
    }

    ~WsConnImpl() = default;

    // 禁用拷贝
    WsConnImpl(const WsConnImpl&) = delete;
    WsConnImpl& operator=(const WsConnImpl&) = delete;

    // 启用移动
    WsConnImpl(WsConnImpl&&) = default;
    WsConnImpl& operator=(WsConnImpl&&) = default;

    /**
     * @brief 关闭连接
     */
    auto close() {
        return m_socket.close();
    }

    /**
     * @brief 获取底层Socket引用
     */
    SocketType& socket() { return m_socket; }

    /**
     * @brief 获取RingBuffer引用
     */
    RingBuffer& ringBuffer() { return m_ring_buffer; }

    /**
     * @brief 获取WsReader
     * @param setting WsReaderSetting配置
     * @return WsReaderImpl<SocketType> Reader对象
     */
    WsReaderImpl<SocketType> getReader(const WsReaderSetting& setting = WsReaderSetting()) {
        // use_mask: 客户端需要mask，服务器不需要
        bool use_mask = !m_is_server;
        return WsReaderImpl<SocketType>(m_ring_buffer, setting, m_socket, m_is_server, use_mask);
    }

    /**
     * @brief 获取WsWriter
     * @param setting WsWriterSetting配置
     * @return WsWriterImpl<SocketType> Writer对象
     */
    WsWriterImpl<SocketType> getWriter(WsWriterSetting setting) {
        // 客户端需要mask，服务器不需要
        setting.use_mask = !m_is_server;
        return WsWriterImpl<SocketType>(setting, m_socket);
    }

    auto echoOnce(std::string& message,
                  WsOpcode& opcode,
                  const WsReaderSetting& reader_setting = WsReaderSetting(),
                  WsWriterSetting writer_setting = WsWriterSetting::byServer()) {
        ++m_echo_counters.composite_awaitables_started;
        using ResultType = std::expected<bool, WsError>;
        if constexpr (is_ssl_socket_v<SocketType>) {
#ifdef GALAY_HTTP_SSL_ENABLED
            return galay::ssl::SslAwaitableBuilder<ResultType>::fromStateMachine(
                       m_socket.controller(),
                       &m_socket,
                       detail::WsSslEchoMachine<SocketType>(this, reader_setting, writer_setting, message, opcode))
                .build();
#else
            static_assert(!sizeof(SocketType), "SSL support is disabled");
#endif
        } else {
            return AwaitableBuilder<ResultType>::fromStateMachine(
                       m_socket.controller(),
                       detail::WsEchoMachine<SocketType>(this, reader_setting, writer_setting, message, opcode))
                .build();
        }
    }

    /**
     * @brief 是否为服务器端连接
     */
    bool isServer() const { return m_is_server; }

    // 允许WsServerImpl访问私有成员
    template<typename S>
    friend class WsServerImpl;
    friend struct detail::WsEchoMachine<SocketType>;
    friend struct detail::WsSslEchoMachine<SocketType>;

private:
    SocketType m_socket;
    RingBuffer m_ring_buffer;
    bool m_is_server;
    EchoCounters m_echo_counters;
};

// 类型别名 - WebSocket over TCP
using WsConn = WsConnImpl<TcpSocket>;

} // namespace galay::websocket

#ifdef GALAY_HTTP_SSL_ENABLED
#include "galay-ssl/async/SslSocket.h"
namespace galay::websocket {
using WssConn = WsConnImpl<galay::ssl::SslSocket>;
} // namespace galay::websocket
#endif

#endif // GALAY_WS_CONN_H
