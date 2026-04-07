//
#pragma once
#include <cstdint>
#include <cstring>
#include <mutex>
#include <algorithm>
#include <stdexcept>
#include <type_traits>

// 템플릿 기본 매개변수(Default Template Argument)
struct NoLock
{
    void lock() {}
    void unlock() {}
};

struct MutexLock
{
    std::mutex _mutex;
    void lock() { _mutex.lock(); }
    void unlock() { _mutex.unlock(); }
};


template<typename LockPolicy = NoLock>
class CRingBufferT
{
public:
    explicit CRingBufferT(size_t capacity = 65536)
        : _capacity(capacity)
        , _readPos(0)
        , _writePos(0)
        , _buffer(nullptr)
    {
        if (capacity == 0)
            return;

        _buffer = new (std::nothrow) char[_capacity];
    }

    ~CRingBufferT()
    {
        delete[] _buffer;
    }

    CRingBufferT(const CRingBufferT&) = delete;
    CRingBufferT& operator=(const CRingBufferT&) = delete;

    bool IsValid() const
    {
        return _buffer != nullptr;
    }

    // === Public API ===

    size_t Enqueue(const void* data, size_t size)
    {
        if (data == nullptr || size == 0 || _buffer == nullptr)
            return 0;

        _lock.lock();

        size_t freeSize = GetFreeSizeInternal();

        // All-or-Nothing: 전체 크기만큼 공간이 없으면 실패
        if (freeSize < size)
        {
            _lock.unlock();
            return 0;
        }

        // 전체 쓰기 보장
        size_t firstWrite = (std::min)(size, _capacity - _writePos);
        std::memcpy(_buffer + _writePos, data, firstWrite);

        if (size > firstWrite)
        {
            size_t secondWrite = size - firstWrite;
            std::memcpy(_buffer, static_cast<const char*>(data) + firstWrite, secondWrite);
        }

        _writePos = (_writePos + size) % _capacity;

        _lock.unlock();
        return size;
    }

    size_t Dequeue(void* data, size_t size)
    {
        if (data == nullptr || size == 0 || _buffer == nullptr)
            return 0;

        _lock.lock();

        size_t dataSize = GetDataSizeInternal();

        // All-or-Nothing: 요청한 크기만큼 데이터가 없으면 실패
        if (dataSize < size)
        {
            _lock.unlock();
            return 0;
        }

        // 전체 읽기 보장
        size_t firstRead = (std::min)(size, _capacity - _readPos);
        std::memcpy(data, _buffer + _readPos, firstRead);

        if (size > firstRead)
        {
            size_t secondRead = size - firstRead;
            std::memcpy(static_cast<char*>(data) + firstRead, _buffer, secondRead);
        }

        _readPos = (_readPos + size) % _capacity;

        _lock.unlock();
        return size;
    }

    // mutable lock을 사용하여 const 함수에서도 동기화 보장
    size_t Peek(void* data, size_t size) const
    {
        if (data == nullptr || size == 0 || _buffer == nullptr)
            return 0;

        _lock.lock();

        size_t dataSize = GetDataSizeInternal();

        // All-or-Nothing: 요청한 크기만큼 데이터가 없으면 실패
        if (dataSize < size)
        {
            _lock.unlock();
            return 0;
        }

        // 전체 읽기 보장
        size_t firstPeek = (std::min)(size, _capacity - _readPos);
        std::memcpy(data, _buffer + _readPos, firstPeek);

        if (size > firstPeek)
        {
            size_t secondPeek = size - firstPeek;
            std::memcpy(static_cast<char*>(data) + firstPeek, _buffer, secondPeek);
        }

        _lock.unlock();
        return size;
    }

    size_t Consume(size_t size)
    {
        if (size == 0 || _buffer == nullptr)
            return 0;

        _lock.lock();

        size_t dataSize = GetDataSizeInternal();

        // All-or-Nothing: 요청한 크기만큼 데이터가 없으면 실패
        if (dataSize < size)
        {
            _lock.unlock();
            return 0;
        }

        _readPos = (_readPos + size) % _capacity;

        _lock.unlock();
        return size;
    }

    void Clear()
    {
        _lock.lock();
        
        if (_buffer == nullptr)
        {
            _lock.unlock();
            return;
        }
        
        _readPos = 0;
        _writePos = 0;
        _lock.unlock();
    }

    // NoLock(싱글스레드) 전용: 외부에서 크기 조회 가능
    // MutexLock 버전에서는 SFINAE + static_assert 이중 차단
    template<typename U = LockPolicy, std::enable_if_t<std::is_same_v<U, NoLock>, int> = 0>
    size_t GetDataSize() const
    {
        static_assert(std::is_same_v<LockPolicy, NoLock>,
            "GetDataSize()는 싱글스레드(NoLock) 버전 전용입니다. 멀티스레드에서는 IsEmpty()를 사용하세요.");
        return GetDataSizeInternal();
    }

    template<typename U = LockPolicy, std::enable_if_t<std::is_same_v<U, NoLock>, int> = 0>
    size_t GetFreeSize() const
    {
        static_assert(std::is_same_v<LockPolicy, NoLock>,
            "GetFreeSize()는 싱글스레드(NoLock) 버전 전용입니다. 멀티스레드에서는 IsEmpty()를 사용하세요.");
        return GetFreeSizeInternal();
    }

    // 양쪽 버전에서 모두 사용 가능 (MT-safe)
    bool IsEmpty() const
    {
        _lock.lock();
        bool empty = (GetDataSizeInternal() == 0);
        _lock.unlock();
        return empty;
    }

private:
    size_t GetDataSizeInternal() const
    {
        if (_writePos >= _readPos)
            return _writePos - _readPos;
        else
            return _capacity - _readPos + _writePos;
    }

    size_t GetFreeSizeInternal() const
    {
        size_t dataSize = GetDataSizeInternal();
        if (dataSize >= _capacity - 1)
            return 0;
        return _capacity - dataSize - 1;
    }

    char* _buffer;
    size_t _capacity;
    size_t _readPos;
    size_t _writePos;
    mutable LockPolicy _lock;  // ← 템플릿 매개변수!
};

// === Type Aliases (사용 편의성) ===
using CRingBufferST = CRingBufferT<NoLock>;       // 싱글스레드 버전
using CRingBufferMT = CRingBufferT<MutexLock>;    // 멀티스레드 버전 (기본)