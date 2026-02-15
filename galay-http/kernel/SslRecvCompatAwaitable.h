#ifndef GALAY_HTTP_SSL_RECV_COMPAT_AWAITABLE_H
#define GALAY_HTTP_SSL_RECV_COMPAT_AWAITABLE_H

#include "galay-ssl/async/SslSocket.h"
#include <utility>

namespace galay::http
{

/**
 * 兼容层：把新版 galay::ssl::SslRecvAwaitable（CustomAwaitable 状态机）
 * 适配为旧调用点可复用的 IOContext 形态。
 */
class SslRecvCompatAwaitable : public galay::kernel::IOContextBase
{
public:
    explicit SslRecvCompatAwaitable(galay::ssl::SslRecvAwaitable&& impl)
        : m_impl(std::move(impl))
    {
        m_plainBuffer = m_impl.m_plainBuffer;
        m_plainLength = m_impl.m_plainLength;
    }

    SslRecvCompatAwaitable(const SslRecvCompatAwaitable&) = delete;
    SslRecvCompatAwaitable& operator=(const SslRecvCompatAwaitable&) = delete;
    SslRecvCompatAwaitable(SslRecvCompatAwaitable&&) = default;
    SslRecvCompatAwaitable& operator=(SslRecvCompatAwaitable&&) = default;

    IOEventType type() const override {
        if (!m_active) {
            return IOEventType::RECV;
        }
        if (m_impl.m_cursor >= m_impl.m_tasks.size()) {
            return IOEventType::RECV;
        }
        const auto& task = m_impl.m_tasks[m_impl.m_cursor];
        return task.type;
    }

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override {
        if (!prepareInnerRecv()) {
            return m_sslResultSet;
        }

        bool consumed_cqe = false;
        while (auto* task = m_impl.front()) {
            bool done = false;
            if (!consumed_cqe) {
                done = task->context->handleComplete(cqe, handle);
                consumed_cqe = true;
            } else {
                done = task->context->handleComplete(nullptr, handle);
            }
            if (!done) {
                return false;
            }
            m_impl.popFront();
        }

        return finalizeInnerRecv();
    }
#else
    bool handleComplete(GHandle handle) override {
        if (!prepareInnerRecv()) {
            return m_sslResultSet;
        }

        while (auto* task = m_impl.front()) {
            const bool done = task->context->handleComplete(handle);
            if (!done) {
                return false;
            }
            m_impl.popFront();
        }

        return finalizeInnerRecv();
    }
#endif

public:
    char* m_plainBuffer = nullptr;
    size_t m_plainLength = 0;
    std::expected<galay::kernel::Bytes, galay::ssl::SslError> m_sslResult;
    bool m_sslResultSet = false;

private:
    bool prepareInnerRecv() {
        if (m_active) {
            return true;
        }
        if (m_sslResultSet) {
            return false;
        }

        m_impl.m_plainBuffer = m_plainBuffer;
        m_impl.m_plainLength = m_plainLength;
        m_impl.m_sslResultSet = false;
        m_impl.resetTaskQueue();
        m_active = true;
        return true;
    }

    bool finalizeInnerRecv() {
        if (!m_impl.m_sslResultSet) {
            m_sslResult = std::unexpected(
                galay::ssl::SslError::fromOpenSSL(galay::ssl::SslErrorCode::kReadFailed));
            m_sslResultSet = true;
            m_active = false;
            return true;
        }

        m_sslResult = std::move(m_impl.m_sslResult);
        m_sslResultSet = true;
        m_impl.m_sslResultSet = false;
        m_active = false;
        return true;
    }

private:
    galay::ssl::SslRecvAwaitable m_impl;
    bool m_active = false;
};

} // namespace galay::http

#endif // GALAY_HTTP_SSL_RECV_COMPAT_AWAITABLE_H
