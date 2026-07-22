//
#include <iostream>
#include <chrono>
#include <vector>
#include <thread>
#include <iomanip>
#include <atomic>
#include <string>
#include "MemoryPool.h"

// ============================================================================
// 테스트용 오브젝트
// ============================================================================
struct TestObject
{
    int data[16]; // 64바이트

    TestObject(int value = 0)
    {
        for (int i = 0; i < 16; ++i)
        {
            data[i] = value;
        }
    }
};

struct LargeObject
{
    char data[256]; // 256바이트

    LargeObject(char value = 0)
    {
        memset(data, value, sizeof(data));
    }
};

// ============================================================================
// 유틸리티
// ============================================================================
class Timer
{
private:
    std::chrono::high_resolution_clock::time_point m_start;
    const char* m_name;

public:
    Timer(const char* name) : m_name(name)
    {
        m_start = std::chrono::high_resolution_clock::now();
    }

    double ElapsedMicroseconds() const
    {
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::micro>(end - m_start).count();
    }

    double ElapsedMilliseconds() const
    {
        return ElapsedMicroseconds() / 1000.0;
    }
};

void PrintResult(const char* name, double timeUs, size_t count)
{
    double opsPerSec = (count / timeUs) * 1000000.0;
    double nsPerOp = (timeUs * 1000.0) / count;

    std::cout << std::left << std::setw(30) << name
              << std::right << std::setw(12) << std::fixed << std::setprecision(2) << timeUs << " us"
              << std::setw(15) << std::setprecision(0) << opsPerSec << " ops/s"
              << std::setw(10) << std::setprecision(1) << nsPerOp << " ns/op"
              << std::endl;
}

void PrintSeparator()
{
    std::cout << std::string(70, '-') << std::endl;
}

void PrintHeader(const char* title)
{
    std::cout << std::endl;
    std::cout << "=== " << title << " ===" << std::endl;
    PrintSeparator();
}

// ============================================================================
// 싱글 스레드 벤치마크
// ============================================================================
void BenchmarkNewDelete(size_t count)
{
    std::vector<TestObject*> objects;
    objects.reserve(count);

    Timer timer("new/delete");

    // 할당
    for (size_t i = 0; i < count; ++i)
    {
        objects.push_back(new TestObject(static_cast<int>(i)));
    }

    // 해제
    for (auto* obj : objects)
    {
        delete obj;
    }

    PrintResult("new/delete", timer.ElapsedMicroseconds(), count * 2);
}

void BenchmarkMemoryPool(size_t count)
{
    CMemoryPool<TestObject> pool;
    std::vector<TestObject*> objects;
    objects.reserve(count);

    Timer timer("CMemoryPool");

    // 할당
    for (size_t i = 0; i < count; ++i)
    {
        objects.push_back(pool.Alloc(static_cast<int>(i)));
    }

    // 해제
    for (auto* obj : objects)
    {
        pool.Free(obj);
    }

    PrintResult("CMemoryPool", timer.ElapsedMicroseconds(), count * 2);
}

void BenchmarkMemoryPoolRAII(size_t count)
{
    CMemoryPool<TestObject> pool;

    Timer timer("CMemoryPool (RAII)");

    {
        std::vector<CMemoryPool<TestObject>::Ptr> objects;
        objects.reserve(count);

        for (size_t i = 0; i < count; ++i)
        {
            objects.push_back(pool.AllocPtr(static_cast<int>(i)));
        }
        // 스코프 종료 시 자동 해제
    }

    PrintResult("CMemoryPool (RAII)", timer.ElapsedMicroseconds(), count * 2);
}

// ============================================================================
// 혼합 할당/해제 패턴 (실제 사용 시뮬레이션)
// ============================================================================
void BenchmarkMixedNewDelete(size_t count)
{
    std::vector<TestObject*> objects;
    objects.reserve(count / 2);

    Timer timer("new/delete (Mixed)");

    for (size_t i = 0; i < count; ++i)
    {
        if (i % 3 != 0 || objects.empty())
        {
            objects.push_back(new TestObject(static_cast<int>(i)));
        }
        else
        {
            delete objects.back();
            objects.pop_back();
        }
    }

    for (auto* obj : objects)
    {
        delete obj;
    }

    PrintResult("new/delete (Mixed)", timer.ElapsedMicroseconds(), count);
}

void BenchmarkMixedMemoryPool(size_t count)
{
    CMemoryPool<TestObject> pool;
    std::vector<TestObject*> objects;
    objects.reserve(count / 2);

    Timer timer("CMemoryPool (Mixed)");

    for (size_t i = 0; i < count; ++i)
    {
        if (i % 3 != 0 || objects.empty())
        {
            objects.push_back(pool.Alloc(static_cast<int>(i)));
        }
        else
        {
            pool.Free(objects.back());
            objects.pop_back();
        }
    }

    for (auto* obj : objects)
    {
        pool.Free(obj);
    }

    PrintResult("CMemoryPool (Mixed)", timer.ElapsedMicroseconds(), count);
}

// ============================================================================
// 멀티스레드 벤치마크
// ============================================================================
std::atomic<size_t> g_totalNewDeleteOps(0);
std::atomic<size_t> g_totalPoolOps(0);

void ThreadNewDelete(size_t count)
{
    std::vector<TestObject*> objects;
    objects.reserve(count);

    for (size_t i = 0; i < count; ++i)
    {
        objects.push_back(new TestObject(static_cast<int>(i)));
    }

    for (auto* obj : objects)
    {
        delete obj;
    }

    g_totalNewDeleteOps.fetch_add(count * 2, std::memory_order_relaxed);
}

void ThreadMemoryPool(CMemoryPool<TestObject>& pool, size_t count)
{
    std::vector<TestObject*> objects;
    objects.reserve(count);

    for (size_t i = 0; i < count; ++i)
    {
        objects.push_back(pool.Alloc(static_cast<int>(i)));
    }

    for (auto* obj : objects)
    {
        pool.Free(obj);
    }

    g_totalPoolOps.fetch_add(count * 2, std::memory_order_relaxed);
}

void BenchmarkMultiThreadNewDelete(size_t threadCount, size_t countPerThread)
{
    g_totalNewDeleteOps.store(0);
    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    Timer timer("new/delete (MT)");

    for (size_t i = 0; i < threadCount; ++i)
    {
        threads.emplace_back(ThreadNewDelete, countPerThread);
    }

    for (auto& t : threads)
    {
        t.join();
    }

    double elapsed = timer.ElapsedMicroseconds();
    size_t totalOps = g_totalNewDeleteOps.load();

    std::cout << std::left << std::setw(30) << ("new/delete (" + std::to_string(threadCount) + " threads)")
              << std::right << std::setw(12) << std::fixed << std::setprecision(2) << elapsed << " us"
              << std::setw(15) << std::setprecision(0) << (totalOps / elapsed) * 1000000.0 << " ops/s"
              << std::endl;
}

void BenchmarkMultiThreadMemoryPool(size_t threadCount, size_t countPerThread)
{
    g_totalPoolOps.store(0);
    CMemoryPool<TestObject> pool;
    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    Timer timer("CMemoryPool (MT)");

    for (size_t i = 0; i < threadCount; ++i)
    {
        threads.emplace_back(ThreadMemoryPool, std::ref(pool), countPerThread);
    }

    for (auto& t : threads)
    {
        t.join();
    }

    double elapsed = timer.ElapsedMicroseconds();
    size_t totalOps = g_totalPoolOps.load();

    std::cout << std::left << std::setw(30) << ("CMemoryPool (" + std::to_string(threadCount) + " threads)")
              << std::right << std::setw(12) << std::fixed << std::setprecision(2) << elapsed << " us"
              << std::setw(15) << std::setprecision(0) << (totalOps / elapsed) * 1000000.0 << " ops/s"
              << std::endl;
}

// ============================================================================
// 경합 테스트 (Producer-Consumer 패턴)
// ============================================================================
void BenchmarkContention(size_t threadCount, size_t iterations)
{
    CMemoryPool<TestObject> pool;
    std::atomic<bool> start(false);
    std::atomic<size_t> readyCount(0);
    std::vector<std::thread> threads;

    auto worker = [&](size_t id)
    {
        std::vector<TestObject*> localObjects;
        localObjects.reserve(100);

        readyCount.fetch_add(1);
        while (!start.load(std::memory_order_acquire))
        {
            _mm_pause();
        }

        for (size_t i = 0; i < iterations; ++i)
        {
            // 할당
            for (size_t j = 0; j < 10; ++j)
            {
                localObjects.push_back(pool.Alloc(static_cast<int>(i * 10 + j)));
            }

            // 해제
            for (auto* obj : localObjects)
            {
                pool.Free(obj);
            }
            localObjects.clear();
        }
    };

    for (size_t i = 0; i < threadCount; ++i)
    {
        threads.emplace_back(worker, i);
    }

    // 모든 스레드 준비 대기
    while (readyCount.load() < threadCount)
    {
        std::this_thread::yield();
    }

    Timer timer("Contention Test");
    start.store(true, std::memory_order_release);

    for (auto& t : threads)
    {
        t.join();
    }

    double elapsed = timer.ElapsedMicroseconds();
    size_t totalOps = threadCount * iterations * 20; // 10 alloc + 10 free

    std::cout << std::left << std::setw(30) << ("Contention (" + std::to_string(threadCount) + " threads)")
              << std::right << std::setw(12) << std::fixed << std::setprecision(2) << elapsed << " us"
              << std::setw(15) << std::setprecision(0) << (totalOps / elapsed) * 1000000.0 << " ops/s"
              << std::endl;
}

// ============================================================================
// Main
// ============================================================================
int main()
{
    std::cout << "================================================================" << std::endl;
    std::cout << "       CMemoryPool Performance Benchmark" << std::endl;
    std::cout << "================================================================" << std::endl;
    std::cout << "Object size: " << sizeof(TestObject) << " bytes" << std::endl;
    std::cout << "Hardware threads: " << std::thread::hardware_concurrency() << std::endl;

    const size_t SINGLE_THREAD_COUNT = 1000000;
    const size_t MULTI_THREAD_COUNT = 100000;

    // ========================================================================
    // 싱글 스레드 테스트
    // ========================================================================
    PrintHeader("Single Thread - Sequential Alloc/Free");
    BenchmarkNewDelete(SINGLE_THREAD_COUNT);
    BenchmarkMemoryPool(SINGLE_THREAD_COUNT);
    BenchmarkMemoryPoolRAII(SINGLE_THREAD_COUNT);

    PrintHeader("Single Thread - Mixed Alloc/Free Pattern");
    BenchmarkMixedNewDelete(SINGLE_THREAD_COUNT);
    BenchmarkMixedMemoryPool(SINGLE_THREAD_COUNT);

    // ========================================================================
    // 멀티 스레드 테스트
    // ========================================================================
    PrintHeader("Multi Thread - Parallel Alloc/Free");

    size_t maxThreads = std::thread::hardware_concurrency();
    if (maxThreads == 0) maxThreads = 4;

    for (size_t threads = 1; threads <= maxThreads; threads *= 2)
    {
        BenchmarkMultiThreadNewDelete(threads, MULTI_THREAD_COUNT);
        BenchmarkMultiThreadMemoryPool(threads, MULTI_THREAD_COUNT);
        PrintSeparator();
    }

    // ========================================================================
    // 경합 테스트
    // ========================================================================
    PrintHeader("Contention Test (High Frequency Alloc/Free)");

    for (size_t threads = 2; threads <= maxThreads; threads *= 2)
    {
        BenchmarkContention(threads, 10000);
    }

    // ========================================================================
    // 결과 요약
    // ========================================================================
    PrintHeader("Benchmark Complete");
    std::cout << "Press Enter to exit..." << std::endl;
    std::cin.get();

    return 0;
}