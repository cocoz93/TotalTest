//
#pragma once

//=============================================================================
// [테스트 전용 대체 헤더 — 원본이 아님]
//
// 원본 MMOServer/MMOServer/LockFreeConfig.h 는 형제 저장소(cocoz93/LockFree)의
// CExternalTlsFreeList 를 직접 참조한다. 이 테스트의 대상은 CSerialBuffer 하나뿐이고
// 락프리 자료구조는 별도 테스트에서 다루므로, 여기서는 풀링을 new/delete 로 바꿔
// 저장소 의존을 끊는다.
//
//   → SerialBuffer.h / SerialBuffer.cpp 는 한 글자도 고치지 않는다.
//     원본이 보는 이름(LockFree::CExternalTlsFreeList<T>::Alloc/Free)만 맞춰준다.
//
// 부수 효과로 얻는 것: Alloc/Free 호출 횟수가 그대로 카운터에 잡히므로
// 참조카운트(AddRef/SubRef)가 Free 를 정확히 1회 부르는지 검증할 수 있다.
// 실제 TLS 프리리스트를 태운 검증은 LockFree 테스트에서 따로 한다.
//=============================================================================

#include <atomic>

namespace LockFree
{
    template<typename T>
    class CExternalTlsFreeList
    {
    public:
        T* Alloc()
        {
            _allocCount.fetch_add(1, std::memory_order_relaxed);
            _liveCount.fetch_add(1, std::memory_order_relaxed);
            return new T();
        }

        void Free(T* node)
        {
            _freeCount.fetch_add(1, std::memory_order_relaxed);
            _liveCount.fetch_sub(1, std::memory_order_relaxed);
            delete node;
        }

        // === 테스트용 계측 (원본 프리리스트에는 없는 것) ===
        int64_t GetAllocCount() const { return _allocCount.load(std::memory_order_relaxed); }
        int64_t GetFreeCount()  const { return _freeCount.load(std::memory_order_relaxed); }
        int64_t GetLiveCount()  const { return _liveCount.load(std::memory_order_relaxed); }

        void ResetCounters()
        {
            _allocCount.store(0, std::memory_order_relaxed);
            _freeCount.store(0, std::memory_order_relaxed);
            _liveCount.store(0, std::memory_order_relaxed);
        }

    private:
        std::atomic<int64_t> _allocCount{ 0 };
        std::atomic<int64_t> _freeCount{ 0 };
        std::atomic<int64_t> _liveCount{ 0 };
    };
}
