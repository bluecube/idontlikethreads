#include "async.hpp"

namespace idontlikethreads {

Async::~Async() {
    {
        std::unique_lock lock(bookkeeping_mutex);
        quit = true;
        cond.notify_all();
    }

    for (auto& thread: threads)
        thread.join();

    threads.clear(); // Not necessary, but why not
}

void Async::maybe_spawn_thread() {
    if (waiting.size() > parked_threads)
        threads.emplace_back(&Async::thread_entrypoint, this);
    else
        cond.notify_one();
}

void Async::thread_entrypoint()
{
    while (auto task = get_next_task())
    {
        std::unique_lock lock(task_mutex);
        AsyncContext ctx(*this, lock);
        task->run(ctx);
    }
}

std::shared_ptr<detail::FutureBase> Async::get_next_task()
{
    std::unique_lock lock(bookkeeping_mutex);

    ++parked_threads;
    cond.wait(lock, [&]() { return quit || !waiting.empty(); });
    --parked_threads;

    if (quit)
        return {};

    auto ret = std::move(waiting.back());
    waiting.pop_back();
    return ret;
}

}
