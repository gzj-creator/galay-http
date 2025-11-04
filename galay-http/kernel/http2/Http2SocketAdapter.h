#ifndef GALAY_HTTP2_SOCKET_ADAPTER_H
#define GALAY_HTTP2_SOCKET_ADAPTER_H

#include <galay/kernel/async/Socket.h>
#include <galay/common/Error.h>
#include <expected>
#include <variant>
#include <functional>
#include <type_traits>

namespace galay::http
{
    /**
     * @brief HTTP/2 Socket 适配器
     * 
     * 统一 AsyncTcpSocket 和 AsyncSslSocket 的接口，
     * 使得 Http2Reader 和 Http2Writer 可以透明地处理两种类型的 socket
     */
    class Http2SocketAdapter
    {
    public:
        Http2SocketAdapter(AsyncTcpSocket& socket)
            : m_socket(std::ref(socket))
        {
        }
        
        Http2SocketAdapter(AsyncSslSocket& socket)
            : m_socket(std::ref(socket))
        {
        }
        
        // 统一的接收接口
        AsyncResult<std::expected<Bytes, CommonError>> recv(char* data, size_t size)
        {
            return std::visit([data, size](auto&& socket) -> AsyncResult<std::expected<Bytes, CommonError>> {
                using SocketType = std::decay_t<decltype(socket.get())>;
                if constexpr (std::is_same_v<SocketType, AsyncTcpSocket>) {
                    return socket.get().recv(data, size);
                } else {
                    return socket.get().sslRecv(data, size);
                }
            }, m_socket);
        }
        
        // 统一的发送接口
        AsyncResult<std::expected<Bytes, CommonError>> send(Bytes&& bytes)
        {
            return std::visit([&bytes](auto&& socket) -> AsyncResult<std::expected<Bytes, CommonError>> {
                using SocketType = std::decay_t<decltype(socket.get())>;
                if constexpr (std::is_same_v<SocketType, AsyncTcpSocket>) {
                    return socket.get().send(std::move(bytes));
                } else {
                    return socket.get().sslSend(std::move(bytes));
                }
            }, m_socket);
        }
        
    private:
        std::variant<
            std::reference_wrapper<AsyncTcpSocket>,
            std::reference_wrapper<AsyncSslSocket>
        > m_socket;
    };
}

#endif // GALAY_HTTP2_SOCKET_ADAPTER_H

