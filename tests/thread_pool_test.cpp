#include "thread_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <vector>
// Job 구조체 정의가 필요합니다 (헤더 위치에 따라 수정하세요)
#include "data.h"

#pragma pack(push, 1)
// 테스트를 위한 헬퍼 데이터 구조체
struct TestData
{
    std::atomic<int>* counter;
    int value;
};
#pragma pack(pop)

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

    while (counter.load() == 0)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
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
    const int JOB_COUNT = 1'00'000;

    for (int i = 0; i < JOB_COUNT; ++i)
    {
        pool.enqueueJob(job);
    }

    // 모든 작업이 끝날 때까지 대기
    int retries = 0;
    while (counter.load() < JOB_COUNT)
    {
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
    // 1. 설정
    const int NUM_THREADS = 8;
    const int JOBS_PER_THREAD = 100;     // 인덱스당 투입할 작업 수
    const int INCREMENTS_PER_JOB = 1000; // 작업당 증가 횟수
    const int TARGET_COUNT =
        JOBS_PER_THREAD * INCREMENTS_PER_JOB; // 목표: 100,000

    // 2. 데이터 정의 (False Sharing 방지 패딩 포함)
    struct PaddingData
    {
        std::atomic<int> count{0};
        char pad[64 - sizeof(std::atomic<int>)]; // 캐시 라인(64바이트) 채우기
    };

    // atomic은 복사가 불가능하므로, vector가 재할당되지 않도록 미리 공간 확보
    std::vector<PaddingData> inputs(NUM_THREADS);

    // 3. 스레드 풀 생성 (데이터보다 나중에 생성 -> 먼저 파괴됨)
    ThreadPool pool(NUM_THREADS);

    // 4. 워커 함수 정의
    auto WorkerFunc = [](void* raw) {
        auto* d = static_cast<PaddingData*>(raw);
        for (int i = 0; i < INCREMENTS_PER_JOB; ++i)
        {
            // memory_order_relaxed: 카운팅에는 충분하며 가장 빠름
            d->count.fetch_add(1, std::memory_order_relaxed);
        }
    };

    // 5. 작업 투입 (총 800개 작업)
    for (int i = 0; i < NUM_THREADS * JOBS_PER_THREAD; ++i)
    {
        // Round-Robin 방식으로 각 데이터에 일감을 골고루 분배
        Job j{WorkerFunc, &inputs[i % NUM_THREADS]};
        pool.enqueueJob(j);
    }

    // 6. [핵심 수정] 대기 로직 (Busy Wait)
    // 모든 inputs의 count가 목표치에 도달할 때까지 기다림
    bool all_finished = false;
    while (!all_finished)
    {
        all_finished = true;
        for (const auto& data : inputs)
        {
            // 하나라도 목표치 미만이면 아직 안 끝난 것임
            if (data.count.load(std::memory_order_relaxed) < TARGET_COUNT)
            {
                all_finished = false;
                std::this_thread::yield(); // CPU를 점유하지 않도록 양보
                break;                     // 다시 검사 루프 처음으로
            }
        }
    }

    // 7. 검증
    for (int i = 0; i < NUM_THREADS; ++i)
    {
        EXPECT_EQ(inputs[i].count.load(), TARGET_COUNT)
            << "Index " << i << " 데이터 손실 발생!";
    }
}