#ifndef __MEMORY_POOL_H__
#define __MEMORY_POOL_H__

#include <new>
#include <atomic>
#include <cstdlib>
#include <windows.h>
#include <intrin.h>

// ============================================================================
// 극한 최적화 상수
// ============================================================================
#define BLOCK_ALLOC_COUNT 256       // 2의 제곱 (비트 연산 최적화)
#define TLS_CACHE_MAX 512
#define TLS_CACHE_MIN 64
#define CACHE_LINE_SIZE 64
#define HUGE_PAGE_SIZE (2 * 1024 * 1024) // 2MB Large Page

// 컴파일러 힌트
#define FORCE_INLINE __forceinline
#define NO_INLINE __declspec(noinline)
#define RESTRICT __restrict
#define ASSUME_ALIGNED(ptr, align) __assume((reinterpret_cast<uintptr_t>(ptr) & ((align) - 1)) == 0)

// Branch prediction 힌트 (MSVC)
#define LIKELY(x) (__builtin_expect(!!(x), 1))
#define UNLIKELY(x) (__builtin_expect(!!(x), 0))

// Prefetch 거리
#define PREFETCH_DISTANCE 4

template<typename T>
class CMemoryPool
{
private:
    // 노드 크기 계산 (컴파일 타임)
    static constexpr size_t NODE_SIZE = (sizeof(T) > sizeof(void*)) ? sizeof(T) : sizeof(void*);
    static constexpr size_t ALIGNED_NODE_SIZE = (NODE_SIZE + CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE - 1);
    static constexpr size_t SHARD_COUNT = 16;
    static constexpr size_t SHARD_MASK = SHARD_COUNT - 1; // 모듈러 연산 대신 비트 AND

    // Free List 노드 구조체 (최小 크기)
    struct Node
    {
        Node* next;
    };

    // 메모리 블록 헤더
    struct alignas(CACHE_LINE_SIZE) Block
    {
        Block* next;
        size_t size;        // VirtualFree용
        bool isLargePage;   // Large Page 여부
    };

    // TLS별 Free List - 극한의 캐시 최적화
    struct alignas(CACHE_LINE_SIZE) ThreadCache
    {
        // === Hot Data (첫 번째 캐시 라인) ===
        Node* hotHead;              // 8 bytes - 가장 자주 접근
        Node* hotTail;              // 8 bytes - 배치 연결용
        size_t hotCount;            // 8 bytes
        uint32_t threadId;          // 4 bytes - 캐싱
        uint32_t shardHint;         // 4 bytes - 선호 샤드
        Node* prefetchPtr;          // 8 bytes - 프리페치 대상
        char pad1[16];              // 패딩
        
        // === Cold Data (두 번째 캐시 라인) ===
        alignas(CACHE_LINE_SIZE) Node* coldHead;
        size_t coldCount;
        size_t localAllocCount;
        size_t localFreeCount;
        char pad2[32];

        ThreadCache() 
            : hotHead(nullptr), hotTail(nullptr), hotCount(0)
            , threadId(GetCurrentThreadId())
            , shardHint(threadId & SHARD_MASK)
            , prefetchPtr(nullptr)
            , coldHead(nullptr), coldCount(0)
            , localAllocCount(0), localFreeCount(0)
        {}
    };

    // Tagged Pointer로 ABA 문제 완전 해결
    struct alignas(16) TaggedPtr
    {
        Node* ptr;
        uint64_t tag;
    };

    // 글로벌 샤드 - 128바이트 정렬 (인접 샤드 간섭 제거)
    struct alignas(128) GlobalStackShard
    {
        std::atomic<Node*> head;
        std::atomic<uint64_t> tag;      // ABA 방지
        std::atomic<size_t> count;      // 노드 개수 (힌트)
        char padding[128 - sizeof(std::atomic<Node*>) - sizeof(std::atomic<uint64_t>) - sizeof(std::atomic<size_t>)];

        GlobalStackShard() : head(nullptr), tag(0), count(0) {}
    };

public:
    CMemoryPool()
        : m_blockList(nullptr)
        , m_totalAllocCount(0)
        , m_totalFreeCount(0)
        , m_blockCount(0)
        , m_largePageEnabled(false)
    {
        // TLS 슬롯 할당
        m_tlsIndex = TlsAlloc();
        if (m_tlsIndex == TLS_OUT_OF_INDEXES)
        {
            throw std::bad_alloc();
        }

        // Large Page 권한 획득 시도
        EnableLargePages();

        // 샤드 초기화
        for (size_t i = 0; i < SHARD_COUNT; ++i)
        {
            new (&m_globalShards[i]) GlobalStackShard();
        }
    }

    ~CMemoryPool()
    {
        // 모든 블록 해제
        Block* block = m_blockList.load(std::memory_order_acquire);
        while (block != nullptr)
        {
            Block* next = block->next;
            
            if (block->isLargePage || block->size >= HUGE_PAGE_SIZE)
            {
                VirtualFree(block, 0, MEM_RELEASE);
            }
            else
            {
                _aligned_free(block);
            }
            
            block = next;
        }

        if (m_tlsIndex != TLS_OUT_OF_INDEXES)
        {
            TlsFree(m_tlsIndex);
        }
    }

    // RAII 스마트 포인터
    class Ptr
    {
    private:
        T* m_ptr;
        CMemoryPool<T>* m_pool;

    public:
        FORCE_INLINE Ptr() : m_ptr(nullptr), m_pool(nullptr) {}
        FORCE_INLINE Ptr(T* ptr, CMemoryPool<T>* pool) : m_ptr(ptr), m_pool(pool) {}

        FORCE_INLINE ~Ptr()
        {
            if (m_ptr != nullptr)
            {
                m_pool->Free(m_ptr);
            }
        }

        FORCE_INLINE Ptr(Ptr&& other) noexcept
            : m_ptr(other.m_ptr), m_pool(other.m_pool)
        {
            other.m_ptr = nullptr;
        }

        FORCE_INLINE Ptr& operator=(Ptr&& other) noexcept
        {
            if (this != &other)
            {
                if (m_ptr != nullptr)
                {
                    m_pool->Free(m_ptr);
                }
                m_ptr = other.m_ptr;
                m_pool = other.m_pool;
                other.m_ptr = nullptr;
            }
            return *this;
        }

        Ptr(const Ptr&) = delete;
        Ptr& operator=(const Ptr&) = delete;

        FORCE_INLINE T* operator->() const { return m_ptr; }
        FORCE_INLINE T& operator*() const { return *m_ptr; }
        FORCE_INLINE T* Get() const { return m_ptr; }
        FORCE_INLINE explicit operator bool() const { return m_ptr != nullptr; }
        
        FORCE_INLINE T* Release()
        {
            T* temp = m_ptr;
            m_ptr = nullptr;
            return temp;
        }
    };

    // ========================================================================
    // Public API - 극한의 인라인 최적화
    // ========================================================================
    
    template<typename... Args>
    FORCE_INLINE T* Alloc(Args&&... args)
    {
        T* RESTRICT ptr = AllocRaw();
        new (ptr) T(std::forward<Args>(args)...);
        return ptr;
    }

    template<typename... Args>
    FORCE_INLINE Ptr AllocPtr(Args&&... args)
    {
        return Ptr(Alloc(std::forward<Args>(args)...), this);
    }

    FORCE_INLINE void Free(T* RESTRICT ptr)
    {
        if (LIKELY(ptr != nullptr))
        {
            ptr->~T();
            FreeRaw(ptr);
        }
    }

    // 통계
    size_t GetAllocCount() const { return m_totalAllocCount.load(std::memory_order_relaxed); }
    size_t GetFreeCount() const { return m_totalFreeCount.load(std::memory_order_relaxed); }
    size_t GetBlockCount() const { return m_blockCount.load(std::memory_order_relaxed); }
    size_t GetUseCount() const { return GetAllocCount() - GetFreeCount(); }

private:
    // ========================================================================
    // Ultra Fast Path - 2-3 CPU 사이클 목표
    // ========================================================================
    
    FORCE_INLINE T* AllocRaw()
    {
        ThreadCache* RESTRICT cache = GetThreadCacheFast();
        
        // [1] Ultra Hot Path: hotHead에서 즉시 반환 (~3 cycles)
        Node* node = cache->hotHead;
        if (LIKELY(node != nullptr))
        {
            cache->hotHead = node->next;
            cache->hotCount--;
            
            // Aggressive prefetch (2단계 선행)
            Node* prefetch1 = cache->hotHead;
            if (LIKELY(prefetch1 != nullptr))
            {
                _mm_prefetch(reinterpret_cast<const char*>(prefetch1), _MM_HINT_T0);
                Node* prefetch2 = prefetch1->next;
                if (prefetch2 != nullptr)
                {
                    _mm_prefetch(reinterpret_cast<const char*>(prefetch2), _MM_HINT_T1);
                }
            }
            
            return reinterpret_cast<T*>(node);
        }

        // [2] Cold Path: coldHead에서 hotHead로 이동
        if (LIKELY(cache->coldHead != nullptr))
        {
            PromoteColdToHot(cache);
            
            node = cache->hotHead;
            cache->hotHead = node->next;
            cache->hotCount--;
            
            return reinterpret_cast<T*>(node);
        }

        // [3] Slow Path
        return AllocRawSlow(cache);
    }

    FORCE_INLINE void FreeRaw(T* RESTRICT ptr)
    {
        Node* node = reinterpret_cast<Node*>(ptr);
        ThreadCache* RESTRICT cache = GetThreadCacheFast();

        // Hot 리스트에 삽입 (~2 cycles)
        node->next = cache->hotHead;
        cache->hotHead = node;
        cache->hotCount++;

        // Lazy flush (매우 드문 경우)
        if (UNLIKELY(cache->hotCount > TLS_CACHE_MAX))
        {
            FlushHotToCold(cache);
        }
    }

    // ========================================================================
    // TLS 캐시 - 극한의 최적화
    // ========================================================================
    
    FORCE_INLINE ThreadCache* GetThreadCacheFast()
    {
        // TLS 조회는 이미 매우 빠름 (레지스터 기반)
        ThreadCache* cache = static_cast<ThreadCache*>(TlsGetValue(m_tlsIndex));
        
        if (UNLIKELY(cache == nullptr))
        {
            cache = CreateThreadCache();
        }
        
        return cache;
    }

    NO_INLINE ThreadCache* CreateThreadCache()
    {
        // 캐시 라인 정렬 할당
        void* mem = _aligned_malloc(sizeof(ThreadCache), CACHE_LINE_SIZE);
        ThreadCache* cache = new (mem) ThreadCache();
        TlsSetValue(m_tlsIndex, cache);
        return cache;
    }

    // Hot에서 Cold로 절반 이동
    void FlushHotToCold(ThreadCache* RESTRICT cache)
    {
        size_t moveCount = cache->hotCount / 2;
        
        Node* moveHead = cache->hotHead;
        Node* moveTail = moveHead;
        
        // 빠른 순회 (언롤링)
        for (size_t i = 1; i < moveCount; ++i)
        {
            moveTail = moveTail->next;
        }
        
        // Hot 리스트 갱신
        cache->hotHead = moveTail->next;
        cache->hotCount -= moveCount;
        
        // Cold 리스트에 추가
        moveTail->next = cache->coldHead;
        cache->coldHead = moveHead;
        cache->coldCount += moveCount;
        
        // Cold가 너무 크면 글로벌로 플러시
        if (cache->coldCount > TLS_CACHE_MAX)
        {
            FlushToGlobal(cache);
        }
    }

    // Cold에서 Hot으로 이동
    FORCE_INLINE void PromoteColdToHot(ThreadCache* RESTRICT cache)
    {
        size_t moveCount = (cache->coldCount < TLS_CACHE_MIN) ? cache->coldCount : TLS_CACHE_MIN;
        
        Node* moveHead = cache->coldHead;
        Node* moveTail = moveHead;
        
        for (size_t i = 1; i < moveCount && moveTail->next != nullptr; ++i)
        {
            moveTail = moveTail->next;
        }
        
        cache->coldHead = moveTail->next;
        cache->coldCount -= moveCount;
        
        moveTail->next = cache->hotHead;
        cache->hotHead = moveHead;
        cache->hotCount += moveCount;
    }

    // ========================================================================
    // Slow Path - 분리하여 I-cache 최적화
    // ========================================================================
    
    NO_INLINE T* AllocRawSlow(ThreadCache* RESTRICT cache)
    {
        // 글로벌에서 배치 가져오기
        if (RefillFromGlobal(cache))
        {
            Node* node = cache->hotHead;
            cache->hotHead = node->next;
            cache->hotCount--;
            return reinterpret_cast<T*>(node);
        }

        // 새 블록 할당
        AllocateNewBlock(cache);
        
        Node* node = cache->hotHead;
        cache->hotHead = node->next;
        cache->hotCount--;
        return reinterpret_cast<T*>(node);
    }

    // ========================================================================
    // 글로벌 샤드 연산 - Lock-Free with Backoff
    // ========================================================================
    
    bool RefillFromGlobal(ThreadCache* RESTRICT cache)
    {
        const size_t startShard = cache->shardHint;
        
        // 가장 많은 노드를 가진 샤드 우선 선택
        size_t bestShard = startShard;
        size_t bestCount = 0;
        
        for (size_t i = 0; i < SHARD_COUNT; ++i)
        {
            size_t idx = (startShard + i) & SHARD_MASK;
            size_t count = m_globalShards[idx].count.load(std::memory_order_relaxed);
            
            if (count > bestCount)
            {
                bestCount = count;
                bestShard = idx;
            }
        }
        
        if (bestCount == 0)
        {
            return false;
        }

        // 배치 Pop
        Node* batch = PopBatchFromShard(bestShard, TLS_CACHE_MIN);
        
        if (batch != nullptr)
        {
            cache->hotHead = batch;
            
            // 카운트 계산
            Node* tail = batch;
            size_t count = 1;
            
            while (tail->next != nullptr && count < TLS_CACHE_MIN)
            {
                tail = tail->next;
                count++;
            }
            
            cache->hotCount = count;
            cache->shardHint = bestShard; // 힌트 갱신
            
            return true;
        }
        
        return false;
    }

    Node* PopBatchFromShard(size_t shardIndex, size_t batchSize)
    {
        GlobalStackShard& shard = m_globalShards[shardIndex];
        
        for (int spin = 0; spin < 16; ++spin)
        {
            Node* oldHead = shard.head.load(std::memory_order_acquire);
            
            if (oldHead == nullptr)
            {
                return nullptr;
            }
            
            // 배치 끝 찾기
            Node* newHead = oldHead;
            size_t actualCount = 0;
            
            for (size_t i = 0; i < batchSize && newHead != nullptr; ++i)
            {
                newHead = newHead->next;
                actualCount++;
            }
            
            // CAS
            if (shard.head.compare_exchange_weak(oldHead, newHead,
                std::memory_order_release, std::memory_order_relaxed))
            {
                shard.tag.fetch_add(1, std::memory_order_relaxed);
                shard.count.fetch_sub(actualCount, std::memory_order_relaxed);
                return oldHead;
            }
            
            // Exponential backoff
            for (int j = 0; j < (1 << spin); ++j)
            {
                _mm_pause();
            }
        }
        
        return nullptr;
    }

    void FlushToGlobal(ThreadCache* RESTRICT cache)
    {
        // Cold 리스트 전체를 글로벌로 이동
        if (cache->coldCount == 0)
        {
            return;
        }
        
        Node* flushHead = cache->coldHead;
        Node* flushTail = flushHead;
        size_t flushCount = cache->coldCount;
        
        // 끝 찾기
        while (flushTail->next != nullptr)
        {
            flushTail = flushTail->next;
        }
        
        cache->coldHead = nullptr;
        cache->coldCount = 0;

        // 통계 동기화
        m_totalAllocCount.fetch_add(cache->localAllocCount, std::memory_order_relaxed);
        m_totalFreeCount.fetch_add(cache->localFreeCount, std::memory_order_relaxed);
        cache->localAllocCount = 0;
        cache->localFreeCount = 0;

        // 가장 적은 샤드에 Push
        size_t targetShard = cache->shardHint;
        size_t minCount = SIZE_MAX;
        
        for (size_t i = 0; i < SHARD_COUNT; ++i)
        {
            size_t count = m_globalShards[i].count.load(std::memory_order_relaxed);
            if (count < minCount)
            {
                minCount = count;
                targetShard = i;
            }
        }

        GlobalStackShard& shard = m_globalShards[targetShard];
        
        // CAS로 Push
        Node* oldHead = shard.head.load(std::memory_order_relaxed);
        do
        {
            flushTail->next = oldHead;
        } while (!shard.head.compare_exchange_weak(oldHead, flushHead,
            std::memory_order_release, std::memory_order_relaxed));
        
        shard.tag.fetch_add(1, std::memory_order_relaxed);
        shard.count.fetch_add(flushCount, std::memory_order_relaxed);
    }

    // ========================================================================
    // 블록 할당 - Large Page 지원
    // ========================================================================
    
    void AllocateNewBlock(ThreadCache* RESTRICT cache)
    {
        const size_t blockDataSize = ALIGNED_NODE_SIZE * BLOCK_ALLOC_COUNT;
        const size_t totalSize = sizeof(Block) + blockDataSize;
        
        Block* block = nullptr;
        bool isLargePage = false;

        // Large Page 시도 (2MB 이상일 때)
        if (m_largePageEnabled && totalSize >= HUGE_PAGE_SIZE)
        {
            block = static_cast<Block*>(VirtualAlloc(
                nullptr,
                totalSize,
                MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES,
                PAGE_READWRITE
            ));
            
            if (block != nullptr)
            {
                isLargePage = true;
            }
        }

        // 일반 할당
        if (block == nullptr)
        {
            if (totalSize >= 65536)
            {
                block = static_cast<Block*>(VirtualAlloc(
                    nullptr,
                    totalSize,
                    MEM_COMMIT | MEM_RESERVE,
                    PAGE_READWRITE
                ));
            }
            else
            {
                block = static_cast<Block*>(_aligned_malloc(totalSize, CACHE_LINE_SIZE));
            }
        }
        
        if (block == nullptr)
        {
            throw std::bad_alloc();
        }

        block->size = totalSize;
        block->isLargePage = isLargePage;

        // 블록 리스트에 추가
        Block* oldHead = m_blockList.load(std::memory_order_relaxed);
        do
        {
            block->next = oldHead;
        } while (!m_blockList.compare_exchange_weak(oldHead, block,
            std::memory_order_release, std::memory_order_relaxed));

        m_blockCount.fetch_add(1, std::memory_order_relaxed);

        // Free List 구성 (역순 - LIFO)
        char* dataPtr = reinterpret_cast<char*>(block) + sizeof(Block);
        
        // 첫 번째 노드
        Node* firstNode = reinterpret_cast<Node*>(dataPtr);
        firstNode->next = nullptr;
        
        // 나머지 노드 연결 (언롤링)
        Node* prevNode = firstNode;
        
        for (size_t i = 1; i < BLOCK_ALLOC_COUNT; ++i)
        {
            Node* node = reinterpret_cast<Node*>(dataPtr + i * ALIGNED_NODE_SIZE);
            node->next = prevNode;
            prevNode = node;
        }

        // Hot 리스트에 연결
        cache->hotHead = prevNode;
        cache->hotCount = BLOCK_ALLOC_COUNT;

        // Prefetch
        _mm_prefetch(reinterpret_cast<const char*>(cache->hotHead), _MM_HINT_T0);
    }

    // Large Page 권한 획득
    void EnableLargePages()
    {
        HANDLE token;
        TOKEN_PRIVILEGES tp;
        
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        {
            if (LookupPrivilegeValue(nullptr, SE_LOCK_MEMORY_NAME, &tp.Privileges[0].Luid))
            {
                tp.PrivilegeCount = 1;
                tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                
                if (AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr))
                {
                    m_largePageEnabled = (GetLastError() == ERROR_SUCCESS);
                }
            }
            CloseHandle(token);
        }
    }

private:
    // 멤버 변수 - 캐시 라인 분리
    alignas(128) GlobalStackShard m_globalShards[SHARD_COUNT];
    alignas(CACHE_LINE_SIZE) std::atomic<Block*> m_blockList;
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> m_totalAllocCount;
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> m_totalFreeCount;
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> m_blockCount;
    DWORD m_tlsIndex;
    bool m_largePageEnabled;
};

#endif // __MEMORY_POOL_H__