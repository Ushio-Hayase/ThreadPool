//
// Created by UshioHayase on 2025-12-22.
//

#include "thread_pool.h"

#include "fast_random.h"

ThreadPool::ThreadPool() : worker_threads_(active_thread_cnt_)
{
    for (int i = 0; i < active_thread_cnt_; ++i)
        worker_threads_[i] = std::make_unique<WorkerThread>(i, worker_threads_);

    for (int i = 0; i < active_thread_cnt_; ++i)
        worker_threads_[i]->start(stop_source_.get_token());

    pushing_thread_ = std::jthread(&ThreadPool::pushJob, this);
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
    // 워커 스레드에 삽입 시도
    for (int i = 0; i < active_thread_cnt_; ++i)
    {
        if (const size_t idx = getFastRandom() % active_thread_cnt_;
            !worker_threads_[idx]->isFull())
        {
            worker_threads_[idx]->push(job);
            return;
        }
    }

    // 메인 큐 삽입
    while (queue_spinlock_.test_and_set(std::memory_order_acquire))
    {
#if defined(__x86_64__) || defined(_M_X64)
        _mm_pause(); // Intel, AMD CPU 힌트
#elif defined(__aarch64__)
        __asm__ __volatile("yield"); // ARM CPU 힌트
#endif
    }

    // critical section start
    main_job_queue_.push_back(job);
    // critical section end

    queue_spinlock_.clear(std::memory_order_release); // free lock

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

        // Pull from q
        while (queue_spinlock_.test_and_set(std::memory_order_acquire))
        {
            // Spin wait hint
#if defined(__x86_64__) || defined(_M_X64)
            _mm_pause();
#endif
        }

        // --- Critical Section Start ---
        if (!main_job_queue_.empty())
        {
            job = main_job_queue_.front();
            main_job_queue_.pop_front();
            has_job = true;
        }
        // --- Critical Section End ---

        queue_spinlock_.clear(std::memory_order_release);

        // Handling items
        if (has_job)
        {
            // Decreased count because it was taken out.
            remain_item_.fetch_sub(1, std::memory_order_release);

            // Forward to worker
            size_t idx = getFastRandom() % active_thread_cnt_;
            int retry_count = 0;

            while (worker_threads_[idx]->isFull())
            {
                idx = (idx + 1) % active_thread_cnt_;
                if (++retry_count > active_thread_cnt_ * 2)
                {
                    std::this_thread::yield(); // If it's too full, give it a
                                               // break
                    retry_count = 0;
                }
            }
            worker_threads_[idx]->push(job);
        }
    }
}
