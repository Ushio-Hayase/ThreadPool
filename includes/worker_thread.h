//
// Created by UshioHayase on 2025-12-23.
//

#ifndef THREAD_POOL_WORKER_THREAD_H
#define THREAD_POOL_WORKER_THREAD_H
#include "data.h"

#include <array>
#include <condition_variable>
#include <deque>
#include <functional>
#include <optional>
#include <thread>

class WorkerThread
{
    static constexpr size_t MAX_JOB_COUNT = 4096;
    static constexpr size_t MASK = MAX_JOB_COUNT - 1;

    std::array<Job, MAX_JOB_COUNT> queue_{};

    alignas(std::hardware_destructive_interference_size)
        std::atomic<size_t> bottom_{0};
    alignas(std::hardware_destructive_interference_size)
        std::atomic<size_t> top_{0};

    const std::vector<WorkerThread*>& thread_pool_;
    size_t my_index_;

    std::jthread thread_;

  public:
    WorkerThread(uint32_t index, const std::vector<WorkerThread*>& thread_pool);

    bool isFull() const;

    void start(const std::stop_token& st);

    void push(const Job& j);
    void run(std::stop_token st);

  private:
    std::optional<Job> popFront();
    std::optional<Job> popBack();
};

#endif // THREAD_POOL_WORKER_THREAD_H
