//
// Created by UshioHayase on 2025-12-22.
//

#include "thread_pool.h"

#include "fast_random.h"

ThreadPool::ThreadPool() : worker_threads_(active_thread_cnt_)
{
    for (int i = 0; i < active_thread_cnt_; ++i)
        worker_threads_[i] = new WorkerThread(i, worker_threads_);

    for (int i = 0; i < active_thread_cnt_; ++i)
        worker_threads_[i]->start(stop_source_.get_token());

    pushing_thread_ = std::jthread(&ThreadPool::pushJob, this);
}

ThreadPool::ThreadPool(const size_t thread_pool_size)
    : active_thread_cnt_(thread_pool_size), worker_threads_(thread_pool_size)
{
    for (int i = 0; i < active_thread_cnt_; ++i)
        worker_threads_[i] = new WorkerThread(i, worker_threads_);

    for (int i = 0; i < active_thread_cnt_; ++i)
        worker_threads_[i]->start(stop_source_.get_token());

    pushing_thread_ = std::jthread(&ThreadPool::pushJob, this);
}

ThreadPool::~ThreadPool()
{
    stop();
    for (const auto* worker : worker_threads_)
        delete worker;
}

void ThreadPool::enqueueJob(const Job& job)
{
    for (int i = 0; i < active_thread_cnt_; ++i)
    {
        if (const size_t idx = getFastRandom() % active_thread_cnt_;
            !worker_threads_[idx]->isFull())
        {
            worker_threads_[idx]->push(job);
            return;
        }
    }
    main_job_queue_.push_back(job);
    remain_item_ = main_job_queue_.size();
    remain_item_.notify_one();
}

void ThreadPool::stop() { stop_source_.request_stop(); }

void ThreadPool::pushJob()
{
    while (!stop_source_.stop_requested())
    {
        remain_item_.wait(0);

        if (const size_t idx = getFastRandom() % active_thread_cnt_;
            !worker_threads_[idx]->isFull())
        {
            const Job j = main_job_queue_.front();
            main_job_queue_.pop_front();
            worker_threads_[idx]->push(j);
            remain_item_ = main_job_queue_.size();
        }
    }
}
