#include "thread_pool.h"

#include "fast_random.h"

// Delegate constructor
ThreadPool::ThreadPool() : ThreadPool(std::thread::hardware_concurrency() - 1)
{
}

ThreadPool::ThreadPool(const size_t thread_pool_size)
    : active_thread_cnt_(thread_pool_size), worker_threads_(thread_pool_size)
{
    for (int i = 0; i < active_thread_cnt_; ++i)
        worker_threads_[i] = std::make_unique<WorkerThread>(i, worker_threads_);

    for (int i = 0; i < active_thread_cnt_; ++i)
        worker_threads_[i]->start(stop_source_.get_token());

    pushing_thread_ = std::jthread(&ThreadPool::pushJob, this);
}

ThreadPool::~ThreadPool() { stop(); }

void ThreadPool::enqueueJob(const Job& job)
{
    // Try inserting into a worker thread first
    for (int i = 0; i < active_thread_cnt_; ++i)
    {
        const size_t idx = getFastRandom() % active_thread_cnt_;
        if (!worker_threads_[idx]->isFull())
        {
            worker_threads_[idx]->push(job);
            return;
        }
    }

    // Insert into Main Queue
    while (queue_spinlock_.test_and_set(std::memory_order_acquire))
    {
#if defined(__x86_64__) || defined(_M_X64)
        _mm_pause();
#elif defined(__aarch64__)
        __asm__ __volatile("yield");
#endif
    }

    main_job_queue_.push_back(job);

    queue_spinlock_.clear(std::memory_order_release);

    remain_item_.fetch_add(1, std::memory_order_release);
    remain_item_.notify_one();
}

void ThreadPool::stop()
{
    if (pushing_thread_.joinable())
        pushing_thread_.request_stop();
    stop_source_.request_stop();

    remain_item_.fetch_add(1, std::memory_order_release);
    remain_item_.notify_all();

    // Wake up all workers with a dummy job or notification logic
    // (Here, simply iterating is enough to trigger destructors eventually)
    Job job;
    for (auto& worker : worker_threads_)
    {
        worker->push(job);
    }
}

void ThreadPool::pushJob()
{
    auto stop_token = pushing_thread_.get_stop_token();
    while (!stop_token.stop_requested())
    {
        while (remain_item_.load(std::memory_order_acquire) == 0)
        {
            remain_item_.wait(0);
        }

        if (stop_token.stop_requested())
            return;

        Job job;
        bool has_job = false;

        // Pull from Main Queue
        while (queue_spinlock_.test_and_set(std::memory_order_acquire))
        {
#if defined(__x86_64__) || defined(_M_X64)
            _mm_pause();
#endif
        }

        if (!main_job_queue_.empty())
        {
            job = main_job_queue_.front();
            main_job_queue_.pop_front();
            has_job = true;
        }

        queue_spinlock_.clear(std::memory_order_release);

        if (has_job)
        {
            remain_item_.fetch_sub(1, std::memory_order_release);

            size_t idx = getFastRandom() % active_thread_cnt_;
            int retry_count = 0;

            // Retry until we find a non-full worker
            while (worker_threads_[idx]->isFull())
            {
                idx = (idx + 1) % active_thread_cnt_;
                if (++retry_count > active_thread_cnt_ * 2)
                {
                    std::this_thread::yield();
                    retry_count = 0;
                }
            }
            worker_threads_[idx]->push(job);
        }
    }
}