#ifndef GALAY_HTTP_KERNEL_COROUTINE_COMPAT_H
#define GALAY_HTTP_KERNEL_COROUTINE_COMPAT_H

#include "galay-kernel/kernel/Task.h"
#include "galay-kernel/kernel/Scheduler.hpp"

#include <optional>

namespace galay::kernel {

class Coroutine;

class CoroutineRef {
public:
    CoroutineRef() noexcept = default;
    explicit CoroutineRef(TaskRef task) noexcept
        : m_task(std::move(task)) {}

    Scheduler* belongScheduler() const noexcept {
        return m_task.belongScheduler();
    }

    void resume() const noexcept {
        if (m_task.isValid()) {
            detail::requestTaskResume(m_task);
        }
    }

    const TaskRef& taskRefView() const noexcept {
        return m_task;
    }

private:
    TaskRef m_task;
};

class WaitResult {
public:
    WaitResult() noexcept = default;
    explicit WaitResult(Task<void>&& task) noexcept
        : m_task(std::move(task)) {}

    WaitResult(WaitResult&&) noexcept = default;
    WaitResult& operator=(WaitResult&&) noexcept = default;

    WaitResult(const WaitResult&) = delete;
    WaitResult& operator=(const WaitResult&) = delete;

    bool await_ready() const noexcept {
        return !m_task.isValid() || m_task.done();
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        TaskRef waiting_task = handle.promise().taskRefView();
        TaskRef child_task = detail::TaskAccess::taskRef(m_task);
        if (!child_task.isValid()) {
            return false;
        }
        if (m_task.done()) {
            return false;
        }

        detail::inheritTaskRuntime(child_task, detail::taskRuntime(waiting_task));
        auto* scheduler = waiting_task.belongScheduler();
        if (scheduler == nullptr) {
            throw std::runtime_error("awaited coroutine has no scheduler available");
        }

        // `Coroutine::wait()` needs to support two modes:
        // 1. waiting on a fresh coroutine, which still needs scheduling;
        // 2. joining a coroutine that was already started via `spawn(...)`.
        // Re-scheduling an already running coroutine can spuriously resume it
        // while it is blocked on `UnsafeChannel`, which manifests as kTimeout.
        if (child_task.belongScheduler() == nullptr) {
            detail::setTaskScheduler(child_task, scheduler);
            if (!detail::scheduleTaskImmediately(child_task)) {
                throw std::runtime_error("failed to schedule awaited coroutine");
            }
            if (m_task.done()) {
                return false;
            }
        }

        child_task.state()->m_next = std::move(waiting_task);
        return true;
    }

    void await_resume() {
        detail::TaskAccess::takeResult(m_task);
    }

private:
    Task<void> m_task;
};

class Coroutine {
public:
    struct promise_type : TaskPromise<void> {
        Coroutine get_return_object() noexcept {
            auto task = TaskPromise<void>::get_return_object();
            m_ref = CoroutineRef(this->taskRefView());
            return Coroutine(std::move(task));
        }

        CoroutineRef& coroutineRef() noexcept { return m_ref; }
        const CoroutineRef& coroutineRef() const noexcept { return m_ref; }
        CoroutineRef& getCoroutine() noexcept { return m_ref; }
        const CoroutineRef& getCoroutine() const noexcept { return m_ref; }

    private:
        CoroutineRef m_ref;
    };

    Coroutine() noexcept = default;
    explicit Coroutine(Task<void>&& task) noexcept
        : m_task(std::move(task)) {}

    Coroutine(Coroutine&&) noexcept = default;
    Coroutine& operator=(Coroutine&&) noexcept = default;

    Coroutine(const Coroutine&) = delete;
    Coroutine& operator=(const Coroutine&) = delete;

    bool isValid() const { return m_task.isValid(); }
    bool done() const { return m_task.done(); }

    Scheduler* belongScheduler() const noexcept {
        return taskRefView().belongScheduler();
    }

    const TaskRef& taskRefView() const noexcept {
        return detail::TaskAccess::taskRef(m_task);
    }

    CoroutineRef getCoroutine() const noexcept {
        return CoroutineRef(taskRefView());
    }

    void resume() const noexcept {
        if (taskRefView().isValid()) {
            detail::requestTaskResume(taskRefView());
        }
    }

    auto operator co_await() & {
        return m_task.operator co_await();
    }

    auto operator co_await() && {
        return std::move(m_task).operator co_await();
    }

    WaitResult wait() & {
        return WaitResult(std::move(m_task));
    }

    WaitResult wait() && {
        return WaitResult(std::move(m_task));
    }

    operator Task<void>() && {
        return std::move(m_task);
    }

private:
    Task<void> m_task;
};

using PromiseType = Coroutine::promise_type;

inline bool scheduleCoroutine(Scheduler* scheduler, Coroutine coroutine) {
    return scheduleTask(scheduler, static_cast<Task<void>>(std::move(coroutine)));
}

class SpawnAwaitable {
public:
    explicit SpawnAwaitable(TaskRef task) noexcept
        : m_task(std::move(task)) {}

    explicit SpawnAwaitable(Task<void>&& task) noexcept
        : m_owned_task(std::move(task))
        , m_task(detail::TaskAccess::taskRef(m_owned_task)) {}

    bool await_ready() const noexcept { return !m_task.isValid(); }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        auto* scheduler = handle.promise().getCoroutine().belongScheduler();
        if (scheduler == nullptr) {
            return false;
        }
        if (m_task.belongScheduler() == nullptr) {
            detail::setTaskScheduler(m_task, scheduler);
        }
        if constexpr (requires(const Promise& promise) {
            { promise.taskRefView() } -> std::same_as<const TaskRef&>;
        }) {
            detail::inheritTaskRuntime(m_task, detail::taskRuntime(handle.promise().taskRefView()));
        }
        (void)detail::scheduleTaskImmediately(m_task);
        return false;
    }

    void await_resume() const noexcept {}

private:
    Task<void> m_owned_task;
    TaskRef m_task;
};

inline SpawnAwaitable spawn(Coroutine& coroutine) {
    return SpawnAwaitable(coroutine.taskRefView());
}

inline SpawnAwaitable spawn(Coroutine&& coroutine) {
    return SpawnAwaitable(static_cast<Task<void>>(std::move(coroutine)));
}

} // namespace galay::kernel

#endif // GALAY_HTTP_KERNEL_COROUTINE_COMPAT_H
