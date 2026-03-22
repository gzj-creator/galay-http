#ifndef GALAY_HTTP_KERNEL_AWAITABLE_COMPAT_H
#define GALAY_HTTP_KERNEL_AWAITABLE_COMPAT_H

#if defined(__has_include)
#if __has_include("../../../galay-kernel/install-local/include/galay-kernel/kernel/Awaitable.h")
#include "../../../galay-kernel/install-local/include/galay-kernel/kernel/Awaitable.h"
#elif defined(__has_include_next)
#if __has_include_next("galay-kernel/kernel/Awaitable.h")
#include_next "galay-kernel/kernel/Awaitable.h"
#else
#include "/usr/local/include/galay-kernel/kernel/Awaitable.h"
#endif
#else
#include "/usr/local/include/galay-kernel/kernel/Awaitable.h"
#endif
#else
#if defined(__has_include_next)
#if __has_include_next("galay-kernel/kernel/Awaitable.h")
#include_next "galay-kernel/kernel/Awaitable.h"
#else
#include "/usr/local/include/galay-kernel/kernel/Awaitable.h"
#endif
#else
#include "/usr/local/include/galay-kernel/kernel/Awaitable.h"
#endif
#endif

#include <vector>

namespace galay::kernel {

class CustomAwaitable : public SequenceAwaitableBase {
public:
    explicit CustomAwaitable(IOController* controller)
        : SequenceAwaitableBase(controller)
    {
        m_tasks.reserve(4);
    }

    void addTask(IOEventType type, IOContextBase* context) {
        m_tasks.push_back(IOTask{type, context, context});
    }

    IOTask* front() override {
        return empty() ? nullptr : &m_tasks[m_front_index];
    }

    const IOTask* front() const override {
        return empty() ? nullptr : &m_tasks[m_front_index];
    }

    void popFront() override {
        if (!empty()) {
            ++m_front_index;
        }
    }

    bool empty() const override {
        return m_front_index >= m_tasks.size();
    }

#ifdef USE_IOURING
    SequenceProgress prepareForSubmit() override {
        GHandle handle{};
        while (!empty()) {
            auto* task = front();
            if (task == nullptr || task->context == nullptr) {
                popFront();
                continue;
            }
            if (!task->context->handleComplete(nullptr, handle)) {
                return SequenceProgress::kNeedWait;
            }
            popFront();
        }
        return SequenceProgress::kCompleted;
    }

    SequenceProgress onActiveEvent(struct io_uring_cqe* cqe, GHandle handle) override {
        if (empty()) {
            return SequenceProgress::kCompleted;
        }

        auto* task = front();
        if (task == nullptr || task->context == nullptr) {
            popFront();
            return prepareForSubmit();
        }

        if (!task->context->handleComplete(cqe, handle)) {
            return SequenceProgress::kNeedWait;
        }

        popFront();
        return prepareForSubmit();
    }
#else
    SequenceProgress prepareForSubmit(GHandle handle) override {
        while (!empty()) {
            auto* task = front();
            if (task == nullptr || task->context == nullptr) {
                popFront();
                continue;
            }
            if (!task->context->handleComplete(handle)) {
                return SequenceProgress::kNeedWait;
            }
            popFront();
        }
        return SequenceProgress::kCompleted;
    }

    SequenceProgress onActiveEvent(GHandle handle) override {
        return prepareForSubmit(handle);
    }
#endif

private:
    std::vector<IOTask> m_tasks;
    size_t m_front_index = 0;
};

} // namespace galay::kernel

#endif // GALAY_HTTP_KERNEL_AWAITABLE_COMPAT_H
