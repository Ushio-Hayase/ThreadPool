//
// Created by UshioHayase on 2025-12-23.
//

#include "worker_thread.h"

#include "fast_random.h"

#include <utility>

WorkerThread::WorkerThread(
    size_t index, const std::vector<std::unique_ptr<WorkerThread>>& thread_pool)
    : thread_pool_(thread_pool), my_index_(index)
{
}

bool WorkerThread::isFull() const { return (bottom_ - top_) >= 4096; }

void WorkerThread::start(const std::stop_token& st)
{
    thread_ = std::jthread(&WorkerThread::run, this, std::move(st));
}

void WorkerThread::push(const Job j)
{
    // Spinlock acquisition (TAS)
    while (queue_lock_.test_and_set(std::memory_order_acquire))
    {
#if defined(__x86_64__) || defined(_M_X64)
        _mm_pause();
#endif
    }

    // --- Critical Section ---
    size_t b = bottom_.load(std::memory_order_relaxed);
    queue_[b & MASK] = j;
    bottom_.store(b + 1, std::memory_order_release);
    // ------------------------

    queue_lock_.clear(std::memory_order_release);

    // Waking up a waiting run()
    bottom_.notify_one();
}
void WorkerThread::run(std::stop_token st)
{
    const size_t thread_pool_size = thread_pool_.size();

    while (!st.stop_requested())
    {
        if (top_ < bottom_)
        {
            if (auto item = popBack(); item.has_value())
            {
                auto [func, data] = item.value();
                func(data);
            }
            continue;
        }

        bool stole_work = false;
        for (int i = 0; i < thread_pool_size; ++i)
        {
            const uint32_t tgt_idx = getFastRandom() % thread_pool_size;

            if (tgt_idx == my_index_)
                continue; // Skip my number

            if (auto item = thread_pool_[tgt_idx]->popFront(); item.has_value())
            {
                auto [func, data] = item.value();
                func(data);
                stole_work = true;
                break;
            }
        }

        // break if nothing to take
        if (stole_work)
            continue;

        // Take a note of the bottom value at the current point in time.
        size_t snapshot_bottom = bottom_.load(std::memory_order_acquire);

        // Immediately after taking the snapshot, compare it to TOP to see if
        // the queue is truly empty.
        size_t current_top = top_.load(std::memory_order_acquire);

        if (current_top < snapshot_bottom)
            continue;

        bottom_.wait(snapshot_bottom);
    }
}

std::optional<Job> WorkerThread::popBack()
{
    std::optional<Job> result = std::nullopt;

    // 락 획득
    while (queue_lock_.test_and_set(std::memory_order_acquire))
    {
#if defined(__x86_64__) || defined(_M_X64)
        _mm_pause();
#endif
    }

    // --- Critical Section ---
    size_t b = bottom_.load(std::memory_order_relaxed) - 1;
    bottom_.store(b, std::memory_order_relaxed);

    std::atomic_thread_fence(
        std::memory_order_seq_cst); // Ensuring order with stealers

    size_t t = top_.load(std::memory_order_relaxed);

    if (t <= b)
    {
        result = queue_[b & MASK];

        // Compete with Stealer when the last 1 is left
        if (t == b)
        {
            if (!top_.compare_exchange_strong(t, t + 1,
                                              std::memory_order_seq_cst,
                                              std::memory_order_relaxed))
            {
                // Failed: Stealer took over -> nullopt after revert
                bottom_.store(b + 1, std::memory_order_relaxed);
                result = std::nullopt;
            }
            else
            {
                // Success: Restore to My -> Empty state
                bottom_.store(b + 1, std::memory_order_relaxed);
            }
        }
    }
    else
    {
        // Empty -> Reset
        bottom_.store(b + 1, std::memory_order_relaxed);
    }
    // ------------------------

    queue_lock_.clear(std::memory_order_release);
    return result;
}

std::optional<Job> WorkerThread::popFront()
{
    size_t t = top_.load(std::memory_order_acquire);

    // Paired with fence in popBack,
    // need to ensure the order of reading bottom after reading top.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    size_t b = bottom_.load(std::memory_order_acquire);

    if (t < b)
    {
        Job job = queue_[t & MASK];

        // Attempt to get item by incrementing top with CAS
        // On success: top becomes t+1 (logically popped)
        // On failure: Another thief took it, or the owner popped the last one
        // with popBack, etc. Take.
        if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                          std::memory_order_relaxed))
        {
            return std::nullopt;
        }

        return job;
    }

    return std::nullopt;
}
