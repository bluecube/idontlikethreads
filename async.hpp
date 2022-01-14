#pragma once

#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <optional>
#include <stdexcept>
#include <type_traits>

namespace idontlikethreads {

class Async;
class AsyncContext;

namespace detail {

/// Base typeless class for futures.
/// This is roughly equivalent to combination of std::future, std::promise and std::packaged_function
class FutureBase {
public:
    FutureBase(const FutureBase&) = delete;
    FutureBase& operator=(const FutureBase&) = delete;

    bool has_value(); // TODO impl
    bool has_exception(); // TODO impl

    void wait();
    //bool wait_for(...)
    //bool wait_until(...)

protected:
    bool throw_if_exception(); // TODO impl
    void set_value(); // TODO impl
    void set_exception(std::exception& e); // TODO impl
    void set_unknown_exception(); // TODO impl

    virtual void run(AsyncContext&) = 0;

    friend class idontlikethreads::Async;
};

}

/// Main typed interface to futures
template <typename T>
class Future: detail::FutureBase {
public:
    using ValueType = T;

    /// Wait for the future to finish and return its value or throw its exception
    ValueType& get() & {
        throw_if_exception();
        return value.value;
    }

    /// Wait for the future to finish and return its value or throw its exception
    const ValueType& get() const & {
        throw_if_exception();
        return value.value;
    }

    /// Wait for the future to finish and return its value or throw its exception
    ValueType&& get() && {
        throw_if_exception();
        return std::move(value.value);
    }

    /// Wait for the future to finish and return its value or throw its exception
    const ValueType&& get() const && {
        throw_if_exception();
        return std::move(value.value);
    }

protected:
    std::optional<ValueType> value;
};

namespace detail {

/// Future that wraps a specific function to be called.
template <typename F>
class FunctionFuture: Future<std::invoke_result<F, AsyncContext&>::type> {
protected:
    FunctionFuture(F func)
    : func(std::move(func)) {}

    virtual void run(AsyncContext&) final
    {
        try {
            value = func();
            set_value();
        } catch (std::exception& e) {
            set_exception(e);
        } catch (...) {
            set_unknown_exception();
        }
    }

    F func;
};


/// RAII wrapper around lock.unlock() followed by lock.lock()
template <typename Lock>
class ScopedUnlock {
public:
    ScopedUnlock(Lock& lock)
    : lock(lock)
    {
        lock.unlock();
    }

    ~ScopedUnlock()
    {
        lock.lock();
    }
private:
    Lock& lock;
};

}

class Async {
public:
    ~Async();

    /// Schedule a task to be run sometime soon.
    /// This is thread safe and can be called from outside of the Async system.
    /// Use run_soon(...).get() to run a single task and wait for it to terminate.
    /// The task should not block outside of AsyncContext::await() calls,
    /// as this would cause all the tasks to block and loose the whole async behavior.
    template <typename F>
    std::shared_ptr<Future<std::invoke_result<F, AsyncContext&>::type>> run_soon(F task)
    {
        std::unique_lock lock(bookkeeping_mutex);
        const auto future = std::make_shared<detail::FunctionFuture<F>>(std::move(task));
        waiting.push_back(future);
        maybe_spawn_thread();
        return future;
    }

private:
    /// Check if there are enough threads parked for the new task and either
    /// spawn a new thread, or wake up one of the parked threads.
    /// Assumes bookkeeping lock is already held.
    void maybe_spawn_thread();

    /// Function running in the worker threads.
    /// Assumes bookkeeping lock is already held.
    void thread_entrypoint();

    /// Return next task to run, or null pointer if the worker thread is supposed to quit
    /// Locks internally.
    std::shared_ptr<detail::FutureBase> get_next_task();

    /// All threads used by Async. Both running and parked.
    std::vector<std::thread> threads;

    /// Futures of tasks waiting to be assigned to a thread. This vector should mostly be empty
    std::vector<std::shared_ptr<detail::FutureBase>> waiting;

    /// Number of threads that are currently parked without a task running
    std::size_t parked_threads = 0;

    std::mutex bookkeeping_mutex;
    std::mutex task_mutex;
    std::condition_variable cond;

    bool quit = false;
};


/// Main interface for running threads to communicate with the Async system.
/// Reference to AsyncContext is passed to every task.
class AsyncContext {
public:
    /// Unlock the task mutex and run func, then re-lock the mutex and pass through
    /// the return value.
    /// This is an escape hatch from Async world.
    template <typename F>
    auto await(F& func)
    {
        detail::ScopedUnlock<std::unique_lock<std::mutex>> unlock(task_lock);
        return func();
    }

    template <typename T>
    T await(std::shared_ptr<Future<T>>& future)
    {
        detail::ScopedUnlock<std::unique_lock<std::mutex>> unlock(task_lock);
        return future->get();
    }

    /// Schedule a task to be run sometime soon.
    /// Just forwarding Async::run_soon() to make it more accessible.
    /// Don't call `ctx.await(ctx.run_soon(f))`, use `f(ctx)` instead.
    template <typename F>
    std::shared_ptr<Future<std::invoke_result<F, AsyncContext&>::type>> run_soon(F task)
    {
        return async.run_soon(std::move(task));
    }

private:
    AsyncContext(Async& async, std::unique_lock<std::mutex>& task_lock)
    : async(async),
      task_lock(task_lock) {}

    Async& async;
    std::unique_lock<std::mutex>& task_lock;

    friend class Async;
};

}
