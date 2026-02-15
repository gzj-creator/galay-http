#ifndef GALAY_HTTP_SSL_HANDSHAKE_AWAITABLE_H
#define GALAY_HTTP_SSL_HANDSHAKE_AWAITABLE_H

#include "galay-kernel/kernel/Coroutine.h"
#include <expected>
#include <optional>
#include <type_traits>
#include <utility>

namespace galay::http
{

/**
 * @brief SSL 握手完整等待体
 * @details 内部消化可重试错误（如 WANT_READ/WANT_WRITE），直到握手完成或出现最终错误才返回
 */
template<typename SocketType>
class SslHandshakeCompleteAwaitable
{
public:
    using HandshakeAwaitableType = decltype(std::declval<SocketType&>().handshake());
    using HandshakeResultType = decltype(std::declval<HandshakeAwaitableType>().await_resume());
    using ErrorType = typename HandshakeResultType::error_type;

    explicit SslHandshakeCompleteAwaitable(SocketType* socket)
        : m_socket(socket)
    {
    }

    bool await_ready() const noexcept {
        return m_socket != nullptr && m_socket->isHandshakeCompleted();
    }

    template<typename Handle>
    bool await_suspend(Handle handle) {
        if (!m_started) {
            m_started = true;
            m_flow = runFlow(this);
        }
        return m_flow.wait().await_suspend(handle);
    }

    HandshakeResultType await_resume() {
        if (m_error.has_value()) {
            return std::unexpected(std::move(*m_error));
        }
        return {};
    }

private:
    static ::galay::kernel::Coroutine runFlow(SslHandshakeCompleteAwaitable* self) {
        if (self->m_socket == nullptr) {
            co_return;
        }

        while (!self->m_socket->isHandshakeCompleted()) {
            auto handshake_result = co_await self->m_socket->handshake();
            if (handshake_result) {
                continue;
            }

            auto error = handshake_result.error();
            if constexpr (requires(const ErrorType& e) { { e.needsRetry() } -> std::convertible_to<bool>; }) {
                if (error.needsRetry()) {
                    continue;
                }
            }

            self->m_error = std::move(error);
            co_return;
        }

        co_return;
    }

    SocketType* m_socket;
    bool m_started = false;
    std::optional<ErrorType> m_error;
    ::galay::kernel::Coroutine m_flow;
};

template<typename SocketType>
inline SslHandshakeCompleteAwaitable<SocketType> handshakeCompletely(SocketType& socket) {
    return SslHandshakeCompleteAwaitable<SocketType>(&socket);
}

} // namespace galay::http

#endif // GALAY_HTTP_SSL_HANDSHAKE_AWAITABLE_H
