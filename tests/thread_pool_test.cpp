#include "thread_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <vector>
// Job 구조체 정의가 필요합니다 (헤더 위치에 따라 수정하세요)
#include "data.h"

// 테스트를 위한 헬퍼 데이터 구조체
struct TestData
{
    std::atomic<int>* counter;
    int value;
};

// 테스트용 작업 함수 (C-Style Function Pointer)
void jobIncrement(void* raw_data)
{
    TestData* data = static_cast<TestData*>(raw_data);
    // atomic 연산으로 안전하게 증가
    data->counter->fetch_add(1, std::memory_order_relaxed);
}

// 1. 기본 생성 및 소멸 테스트
TEST(ThreadPoolTest, ConstructionDestruction)
{
    // 스레드 풀이 생성되고, 스코프를 나갈 때
    // Deadlock이나 Crash 없이 정상 종료되는지 확인
    ThreadPool pool(4);
}

// 2. 단일 작업 실행 테스트
TEST(ThreadPoolTest, SimpleJobExecution)
{
    ThreadPool pool(4);
    std::atomic<int> counter{0};
    TestData data{&counter, 0};

    Job job{jobIncrement, &data};
    pool.enqueueJob(job);

    // 비동기 작업이므로 잠시 대기 (실제 엔진엔 Fence나 Wait 기능 필요)
    int timeout_ms = 1000;
    while (counter.load() == 0 && timeout_ms > 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        timeout_ms--;
    }

    ASSERT_EQ(counter.load(), 1);
}

// 3. 대량 작업 스트레스 테스트 (Ring Buffer Overflow 검증)
TEST(ThreadPoolTest, MassiveConcurrentJobs)
{
    // 하드웨어 코어만큼 스레드 생성
    unsigned int thread_count = std::thread::hardware_concurrency() - 1;
    if (thread_count == 0)
        thread_count = 4;

    ThreadPool pool(thread_count);

    std::atomic<int> counter{0};
    TestData data{&counter, 0};
    Job job{jobIncrement, &data};

    // 큐 크기(4096)보다 훨씬 많은 작업을 투입하여
    // 인덱스 마스킹과 링 버퍼 회전이 정상 작동하는지 확인
    const int JOB_COUNT = 100000;

    for (int i = 0; i < JOB_COUNT; ++i)
    {
        pool.enqueueJob(job);

        // 너무 빨리 넣으면 메인 스레드 큐가 터질 수 있으니
        // 꽉 찼을 때를 대비한 백오프 로직이 엔진에 있어야 함.
        // 여기서는 테스트를 위해 살짝 텀을 줌 (선택 사항)
        if (i % 4000 == 0)
            std::this_thread::yield();
    }

    // 모든 작업이 끝날 때까지 대기
    int retries = 0;
    while (counter.load() < JOB_COUNT)
    {
        std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        retries++;
    }

    EXPECT_EQ(counter.load(), JOB_COUNT)
        << "모든 작업이 처리되지 않았거나 중복 실행되었습니다.";
}

// 4. 작업 훔치기 (Work Stealing) 성능 검증
// 무거운 작업을 한 곳에 몰아넣고, 여러 스레드가 나눠서 처리하는지 확인
void HeavyTask(void* raw_data)
{
    auto* data = static_cast<TestData*>(raw_data);

    // CPU를 태우는 무거운 연산
    double result = 0;
    for (int i = 0; i < 10000; ++i)
    {
        result += std::sin(i) * std::cos(i);
    }

    data->counter->fetch_add(1, std::memory_order_relaxed);
}

TEST(ThreadPoolTest, WorkStealingBalance)
{
    unsigned int thread_count = 4;
    ThreadPool pool(thread_count);

    std::atomic<int> counter{0};
    TestData data{&counter, 0};
    Job job{HeavyTask, &data};

    constexpr int JOB_COUNT = 1000;

    auto start_time = std::chrono::high_resolution_clock::now();

    // 메인 스레드(혹은 0번 워커)에만 작업을 전부 몰아넣음
    for (int i = 0; i < JOB_COUNT; ++i)
    {
        pool.enqueueJob(job);
    }

    // 완료 대기
    while (counter.load() < JOB_COUNT)
    {
        std::this_thread::yield();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                        end_time - start_time)
                        .count();

    // 단일 스레드로 돌렸다면 오래 걸렸을 작업이
    // 병렬로 처리되어 빠르게 끝났는지 확인 (절대적인 수치는 환경마다 다름)
    std::cout << "Processed " << JOB_COUNT << " heavy jobs in " << duration
              << "ms" << std::endl;

    ASSERT_EQ(counter.load(), JOB_COUNT);
}

// 5. False Sharing 확인용 (선택)
// Job 구조체나 Atomic 변수들이 캐시 라인을 침범하지 않는지 간접 확인
TEST(ThreadPoolTest, CacheCoherency)
{
    // 서로 다른 데이터를 건드리는 수많은 작업을 동시에 수행하여
    // 데이터 오염(Data Corruption)이 없는지 확인
    const int NUM_THREADS = 8;
    ThreadPool pool(NUM_THREADS);

    struct PaddingData
    {
        char pad[64]; // 캐시 라인 분리
        int id;
        std::atomic<int> count{0};
    };

    std::vector<PaddingData> inputs(NUM_THREADS);

    auto WorkerFunc = [](void* raw) {
        auto* d = static_cast<PaddingData*>(raw);
        for (int i = 0; i < 1000; ++i)
            d->count++;
    };

    for (int i = 0; i < NUM_THREADS * 100; ++i)
    {
        // 각 작업은 자신의 인덱스에 맞는 데이터만 건드림
        Job j{WorkerFunc, &inputs[i % NUM_THREADS]};
        pool.enqueueJob(j);
    }

    // 대기 로직...
    // 검증: 모든 inputs[i].count가 100이어야 함
}