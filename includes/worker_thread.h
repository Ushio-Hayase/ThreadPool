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

#ifdef __cpp_lib_hardware_interference_size
using std::hardware_destructive_interference_size;
#else
constexpr std::size_t hardware_destructive_interference_size = 64;
#endif

class WorkerThread
{
    static constexpr size_t MAX_JOB_COUNT = 4096;
    static constexpr size_t MASK = MAX_JOB_COUNT - 1;

    std::atomic_flag queue_lock_ = ATOMIC_FLAG_INIT;

    const std::vector<std::unique_ptr<WorkerThread>>& thread_pool_;
    size_t my_index_;

    std::jthread thread_;

    alignas(std::hardware_destructive_interference_size)
        std::atomic<int64_t> bottom_{0};
    alignas(std::hardware_destructive_interference_size)
        std::atomic<int64_t> top_{0};

    std::array<Job, MAX_JOB_COUNT> queue_{};

  public:
    WorkerThread(size_t index,
                 const std::vector<std::unique_ptr<WorkerThread>>& thread_pool);
    ~WorkerThread() = default;

    bool isFull() const;

    void start(const std::stop_token& st);

    void push(const Job j);
    void run(std::stop_token st);

  private:
    std::optional<Job> popFront();
    std::optional<Job> popBack();
};

#endif // THREAD_POOL_WORKER_THREAD_H
