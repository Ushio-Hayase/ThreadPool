//
// Created by UshioHayase on 2025-12-23.
//

#include "worker_thread.h"

#include "fast_random.h"

#include <utility>

WorkerThread::WorkerThread(uint32_t index,
                           const std::vector<WorkerThread*>& thread_pool)
    : thread_pool_(thread_pool), my_index_(index)
{
}

bool WorkerThread::isFull() const { return (bottom_ & MASK) < (top_ & MASK); }

void WorkerThread::start(const std::stop_token& st)
{
    thread_ = std::jthread(&WorkerThread::run, this, st);
}

void WorkerThread::push(const Job& j)
{
    size_t b = bottom_.load(std::memory_order_relaxed);

    queue_[b & MASK] = j;

    std::atomic_thread_fence(std::memory_order_release);

    bottom_.store(b + 1, std::memory_order_release);
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
                continue; // 내 번호는 스킵

            if (auto item = thread_pool_[tgt_idx]->popFront(); item.has_value())
            {
                auto [func, data] = item.value();
                func(data);
                stole_work = true;
                break;
            }
        }
        // 가져갈 것이 없으면 휴식
        if (!stole_work)
            std::this_thread::yield();
    }
}

std::optional<Job> WorkerThread::popBack()
{
    size_t b = bottom_.load(std::memory_order_relaxed) - 1;

    bottom_.store(b, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);

    size_t t = top_.load(std::memory_order_acquire);

    if (t <= b)
    {
        Job job = queue_[b & MASK];

        // [위험구간] 잡이 딱 1개 남았었을 경우
        if (t == b)
        {
            // Top이 여전히 t라면 -> t+1로 증가시키고 성공 (Compare-And-Swap)
            // 실패했다면 -> 채간 것
            if (!top_.compare_exchange_strong(t, t + 1,
                                              std::memory_order_seq_cst,
                                              std::memory_order_relaxed))
            {
                // 실패시 되돌려두고 nullopt 반환
                bottom_.store(b + 1, std::memory_order_relaxed);
                return std::nullopt;
            }
            // 성공시 작업 반환
            return job;
        }
        return job;
    }

    // 비어있으면 줄였던 bottom_ 복구 및 nullopt 반환
    bottom_.store(b + 1, std::memory_order_relaxed);
    return std::nullopt;
}

std::optional<Job> WorkerThread::popFront()
{
    size_t t = top_.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    size_t b = bottom_.load(std::memory_order_acquire);

    if (t < b)
    {
        // 잡이 있어 보임
        Job job = queue_[t & MASK];

        // 가져가기 시도
        // Top을 t에서 t+1로 바꿀 수 있어야 성공
        if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                          std::memory_order_relaxed))
        {
            return std::nullopt; // 실패
        }

        return job;
    }
    return std::nullopt; // 비어있음
}
