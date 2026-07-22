#include "Profiler.h"
#include <iostream>
#include <thread>
#include <vector>

// ============================================================================
// 사용 예시
// ============================================================================


void HeavyComputation()
{
	PROFILE_SCOPE("HeavyComputation");
    volatile double result = 0;
    for (int i = 0; i < 100000; ++i)
    {
        result += i * 0.5;
    }
}

void QuickTask()
{
	PROFILE_SCOPE("QuickTask");
    volatile int sum = 0;
    for (int i = 0; i < 1000; ++i)
    {
        sum += i;
    }
}

void WorkerThread(int threadId, int iterations)
{
    for (int i = 0; i < iterations; ++i)
    {
        {
            PROFILE_SCOPE("HeavyComputation");
            HeavyComputation();
        }
        {
            PROFILE_SCOPE("QuickTask");
            QuickTask();
        }
    }
}

int main()
{
    std::cout << "TLS-based Multithreaded Profiler Demo\n";
    std::cout << "Starting with 4 threads...\n\n";

    const int numThreads = 4;
    const int iterationsPerThread = 50000;

    std::vector<std::thread> threads;
    threads.reserve(numThreads);

    // 멀티스레드 작업 시작
    for (int i = 0; i < numThreads; ++i)
    {
        threads.emplace_back(WorkerThread, i, iterationsPerThread);
    }

    // 모든 스레드 완료 대기
    for (auto& t : threads)
    {
        t.join();
    }

    // 프로파일링 결과 출력
    Profiler::CProfilerManager::Instance().PrintReport();

    return 0;
}