//
// Created by UshioHayase on 2025-12-22.
//

#ifndef THREADPOOL_THREAD_POOL_H
#define THREADPOOL_THREAD_POOL_H

#include "worker_thread.h"

#include <deque>
#include <thread>
#include <vector>

class ThreadPool
{
    const unsigned int active_thread_cnt_ =
        std::thread::hardware_concurrency() - 1;
    std::vector<WorkerThread*> worker_threads_;

    std::deque<Job> main_job_queue_;
    std::atomic<uint32_t> remain_item_ = 0;

    std::jthread pushing_thread_;

    std::stop_source stop_source_;

  public:
    ThreadPool();
    explicit ThreadPool(size_t thread_pool_size);
    ~ThreadPool();

    void stop();

    void enqueueJob(const Job& job);

  private:
    void pushJob();
};

#endif // THREADPOOL_THREAD_POOL_H
