//
// Created by UshioHayase on 2025-12-23.
//

#ifndef THREAD_POOL_DATA_H
#define THREAD_POOL_DATA_H

struct Job
{
    void (*func)(void*); // 실행할 함수 포인터
    void* data;          // 데이터 포인터
};

#endif // THREAD_POOL_DATA_H
