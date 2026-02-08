#include "worker_thread.h"

#include "fast_random.h"

WorkerThread::WorkerThread(
    size_t index, const std::vector<std::unique_ptr<WorkerThread>>& thread_pool)
    : thread_pool_(thread_pool), my_index_(index)
{
}

bool WorkerThread::isFull() const
{
    // Relaxed load is acceptable for a quick check
    return (bottom_.load(std::memory_order_relaxed) -
            top_.load(std::memory_order_relaxed)) >=
           static_cast<int64_t>(MAX_JOB_COUNT);
}

void WorkerThread::start(const std::stop_token& st)
{
    thread_ = std::jthread(&WorkerThread::run, this, st);
}

void WorkerThread::push(const Job j)
{
    // Acquire Lock
    while (queue_lock_.test_and_set(std::memory_order_acquire))
    {
#if defined(__x86_64__) || defined(_M_X64)
        _mm_pause();
#endif
    }

    // Critical Section
    int64_t b = bottom_.load(std::memory_order_relaxed);
    queue_[static_cast<size_t>(b) & MASK] = j;
    bottom_.store(b + 1, std::memory_order_release);

    // Release Lock
    queue_lock_.clear(std::memory_order_release);

    bottom_.notify_one();
}

std::optional<Job> WorkerThread::popBack()
{
    std::optional<Job> result = std::nullopt;

    while (queue_lock_.test_and_set(std::memory_order_acquire))
    {
#if defined(__x86_64__) || defined(_M_X64)
        _mm_pause();
#endif
    }

    int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
    bottom_.store(b, std::memory_order_relaxed);

    std::atomic_thread_fence(std::memory_order_seq_cst);

    int64_t t = top_.load(std::memory_order_relaxed);

    if (t <= b)
    {
        result = queue_[static_cast<size_t>(b) & MASK];

        // Handle race with Stealer when 1 item remains
        if (t == b)
        {
            if (!top_.compare_exchange_strong(t, t + 1,
                                              std::memory_order_seq_cst,
                                              std::memory_order_relaxed))
            {
                bottom_.store(b + 1, std::memory_order_relaxed);
                result = std::nullopt;
            }
            else
            {
                bottom_.store(b + 1, std::memory_order_relaxed);
            }
        }
    }
    else
    {
        bottom_.store(b + 1, std::memory_order_relaxed);
    }

    queue_lock_.clear(std::memory_order_release);
    return result;
}

std::optional<Job> WorkerThread::popFront()
{
    std::optional<Job> result = std::nullopt;

    // Lock is required for popFront as well to prevent torn reads
    while (queue_lock_.test_and_set(std::memory_order_acquire))
    {
#if defined(__x86_64__) || defined(_M_X64)
        _mm_pause();
#endif
    }

    int64_t t = top_.load(std::memory_order_relaxed);
    int64_t b = bottom_.load(std::memory_order_relaxed);

    if (t < b)
    {
        Job job = queue_[static_cast<size_t>(t) & MASK];

        if (top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                         std::memory_order_relaxed))
        {
            result = job;
        }
    }

    queue_lock_.clear(std::memory_order_release);
    return result;
}

void WorkerThread::run(std::stop_token st)
{
    const size_t pool_size = thread_pool_.size();

    while (!st.stop_requested())
    {
        // 1. Process local queue
        if (auto item = popBack(); item.has_value())
        {
            item->func(item->data);
            continue;
        }

        // 2. Steal work
        bool stole_work = false;
        for (size_t i = 0; i < pool_size; ++i)
        {
            if (i == my_index_)
                continue;

            // Random stealing
            const size_t tgt_idx = (my_index_ + i + 1) % pool_size;

            if (auto item = thread_pool_[tgt_idx]->popFront(); item.has_value())
            {
                item->func(item->data);
                stole_work = true;
                break;
            }
        }

        if (stole_work)
            continue;

        // 3. Wait logic (Snapshot -> Double Check -> Wait)
        int64_t snapshot = bottom_.load(std::memory_order_acquire);
        int64_t current_top = top_.load(std::memory_order_acquire);

        if (current_top < snapshot)
            continue;

        bottom_.wait(snapshot);
    }
}