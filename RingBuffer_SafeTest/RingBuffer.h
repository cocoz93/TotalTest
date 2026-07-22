//
#pragma once
#include <cstdint>
#include <cstring>
#include <mutex>
#include <algorithm>
#include <stdexcept>


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

// __________________________________________________________________
// 
// NoLock 싱글스레드 버전
// 싱글스레드 환경에서 사용
// __________________________________________________________________
template<typename LockPolicy = NoLock>
class CRingBufferT
{
public:
    explicit CRingBufferT()
        : _capacity(0)
        , _readPos(0)
        , _writePos(0)
        , _buffer(nullptr)
    {
    }

    // 즉시 할당 편의 생성자 — capacity>0면 Init로 버퍼 확보 (클라 등 풀링 불필요한 곳).
    //   서버 세션 풀은 무인자 ctor(빈) + Init() 사용. 두 생성자는 오버로드라 무충돌.
    explicit CRingBufferT(size_t capacity)
        : _capacity(0)
        , _readPos(0)
        , _writePos(0)
        , _buffer(nullptr)
    {
        Init(capacity);
    }

    ~CRingBufferT()
    {
        delete[] _buffer;
    }

    bool Init(size_t capacity = 65535)
    {
        if (capacity == 0)
            return false;

        _buffer = new (std::nothrow) char[capacity];
        if (_buffer == nullptr)
            return false;

        _capacity = capacity;
        return true;
    }

    bool IsValid() const
    {
        return _buffer != nullptr;
    }

    size_t Enqueue(const void* data, size_t size)
    {
        _lock.lock();

        if (data == nullptr || size == 0 || _buffer == nullptr)
        {
            _lock.unlock();
            return 0;
        }

        size_t freeSize = GetFreeSize_Internal();

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
        _lock.lock();

        if (data == nullptr || size == 0 || _buffer == nullptr)
        {
            _lock.unlock();
            return 0;
        }

        size_t dataSize = GetDataSize_Internal();

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

    size_t Peek(void* data, size_t size) const
    {
        _lock.lock();

        if (data == nullptr || size == 0 || _buffer == nullptr)
        {
            _lock.unlock();
            return 0;
        }

        size_t dataSize = GetDataSize_Internal();

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
        _lock.lock();

        if (size == 0 || _buffer == nullptr)
        {
            _lock.unlock();
            return 0;
        }

        size_t dataSize = GetDataSize_Internal();

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

    size_t MoveWritePtr(size_t size)
    {
        _lock.lock();

        if (size == 0 || _buffer == nullptr)
        {
            _lock.unlock();
            return 0;
        }

        size_t freeSize = GetFreeSize_Internal();

        // 여유 공간 검증
        if (freeSize < size)
        {
            _lock.unlock();
            return 0;
        }

        _writePos = (_writePos + size) % _capacity;

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

    size_t GetDataSize() const
    {
        _lock.lock();
        size_t result = GetDataSize_Internal();
        _lock.unlock();
        return result;
    }

    size_t GetFreeSize() const
    {
        _lock.lock();
        size_t result = GetFreeSize_Internal();
        _lock.unlock();
        return result;
    }

    char* GetWritePtr()
    {
        _lock.lock();
        char* ptr = _buffer + _writePos;
        _lock.unlock();
        return ptr;
    }

    char* GetReadPtr()
    {
        _lock.lock();
        char* ptr = _buffer + _readPos;
        _lock.unlock();
        return ptr;
    }

    size_t GetDirectWriteSize() const
    {
        _lock.lock();
        size_t result;
        if (_writePos >= _readPos)
            result = (_readPos == 0) ? _capacity - _writePos - 1 : _capacity - _writePos;
        else
            result = _readPos - _writePos - 1;
        _lock.unlock();
        return result;
    }

    size_t GetDirectReadSize() const
    {
        _lock.lock();
        size_t result;
        if (_writePos >= _readPos)
            result = _writePos - _readPos;
        else
            result = _capacity - _readPos;
        _lock.unlock();
        return result;
    }

    // 모든 함수는 lock으로 보호되지만, 여러개의 함수를 호출했을때
    // 일관성이 보장되지는 않는다. 따라서 새로운 구조체 추가
    struct SendInfo
    {
        char* readPtr;
        size_t dataSize;
        size_t directReadSize;
    };

    SendInfo GetSendInfo()
    {
        _lock.lock();
        SendInfo info;
        info.readPtr = _buffer + _readPos;
        info.dataSize = GetDataSize_Internal();

        if (_writePos >= _readPos)
            info.directReadSize = _writePos - _readPos;
        else
            info.directReadSize = _capacity - _readPos;

        _lock.unlock();
        return info;
    }

public:
    char* _buffer;
    size_t _capacity;
    size_t _readPos;
    size_t _writePos;
    mutable LockPolicy _lock;

private:
    size_t GetDataSize_Internal() const
    {
        if (_writePos >= _readPos)
            return _writePos - _readPos;
        else
            return _capacity - _readPos + _writePos;
    }

    size_t GetFreeSize_Internal() const
    {
        size_t dataSize = GetDataSize_Internal();
        if (dataSize >= _capacity - 1)
            return 0;
        return _capacity - dataSize - 1;
    }

    CRingBufferT(const CRingBufferT&) = delete;
    CRingBufferT& operator=(const CRingBufferT&) = delete;
};

// === Type Aliases (사용 편의성) ===
using CRingBufferST = CRingBufferT<NoLock>;       // 싱글스레드 버전
using CRingBufferMT = CRingBufferT<MutexLock>;    // 멀티스레드 버전
