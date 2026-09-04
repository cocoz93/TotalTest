//
#pragma once
#include <cstdint>
#include <cstring>
#include <mutex>
#include <algorithm>
#include <stdexcept>
#include <new>          // [Belt 추가] std::nothrow — 원본은 다른 헤더에 업혀 통과하고 있었다


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
        : _buffer(nullptr)          // [Belt] 초기화 순서를 선언 순서와 맞춤 (-Wreorder)
        , _capacity(0)
        , _readPos(0)
        , _writePos(0)
        , _submitPos(0)
        , _ownsBuffer(true)
    {
    }

    // 즉시 할당 편의 생성자 — capacity>0면 Init로 버퍼 확보 (클라 등 풀링 불필요한 곳).
    //   서버 세션 풀은 무인자 ctor(빈) + Init() 사용. 두 생성자는 오버로드라 무충돌.
    explicit CRingBufferT(size_t capacity)
        : _buffer(nullptr)          // [Belt] 초기화 순서를 선언 순서와 맞춤 (-Wreorder)
        , _capacity(0)
        , _readPos(0)
        , _writePos(0)
        , _submitPos(0)
        , _ownsBuffer(true)
    {
        Init(capacity);
    }

    ~CRingBufferT()
    {
        if (_ownsBuffer)
            delete[] _buffer;
    }

    bool Init(size_t capacity = 65535)
    {
        if (capacity == 0)
            return false;

        // [Belt 추가] 재호출 방어 — 원본은 두 번 부르면 이전 버퍼가 샜다.
        //   재초기화를 허용하되(용량 변경) 위치는 전부 0으로 되돌린다.
        if (_buffer != nullptr && _ownsBuffer)
            delete[] _buffer;
        _buffer = nullptr;
        _readPos = 0;
        _writePos = 0;
        _submitPos = 0;

        _buffer = new (std::nothrow) char[capacity];
        if (_buffer == nullptr)
            return false;

        _capacity = capacity;
        _ownsBuffer = true;
        return true;
    }

    // 외부 소유 버퍼로 초기화 — 비소유(소멸 시 delete 안 함). 1회 초기화 전용 (Init와 동일 계약).
    //   RIO 등록 슬랩처럼 링버퍼보다 수명이 긴 메모리의 슬라이스를 링으로 쓸 때 사용.
    bool InitExternal(char* buffer, size_t capacity)
    {
        if (buffer == nullptr || capacity == 0)
            return false;

        _buffer = buffer;
        _capacity = capacity;
        _ownsBuffer = false;
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
        _submitPos = 0;
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

    // ── 다중 pending 송신 지원 ────────────────────────────────────────────
    // 1-pending 시절에는 "미완료 구간"과 "미제출 구간"이 같아서 readPos 하나로 충분했다.
    // 제출을 여러 개 띄우면 둘이 갈라지므로 제출 경계(_submitPos)를 따로 본다.
    struct SubmitInfo
    {
        char*  submitPtr;    // 아직 제출하지 않은 구간의 시작
        size_t size;         // 미제출 총량 (write - submit)
        size_t directSize;   // 그중 링 끝까지 이어지는 직선 구간 (랩 전까지)
    };

    SubmitInfo GetSubmitInfo()
    {
        _lock.lock();
        SubmitInfo info;
        info.submitPtr = _buffer + _submitPos;
        info.size = GetUnsubmittedSize_Internal();

        if (_writePos >= _submitPos)
            info.directSize = _writePos - _submitPos;
        else
            info.directSize = _capacity - _submitPos;

        _lock.unlock();
        return info;
    }

    // 제출한 만큼 경계를 옮긴다. 반드시 실제 제출 "전"에 호출할 것 —
    //   제출 직후 다른 스레드가 완료를 처리할 수 있고, ConsumeSubmitted가 이 경계를 먼저 봐야 한다.
    size_t MarkSubmitted(size_t size)
    {
        _lock.lock();

        if (size == 0 || _buffer == nullptr)
        {
            _lock.unlock();
            return 0;
        }

        if (GetUnsubmittedSize_Internal() < size)
        {
            _lock.unlock();
            return 0;
        }

        _submitPos = (_submitPos + size) % _capacity;

        _lock.unlock();
        return size;
    }

    // 완료된 만큼 읽기 포인터를 옮긴다 — Consume과 달리 "제출된 구간"만 소비할 수 있다.
    //   Consume은 _submitPos를 쓰지 않는 기존 사용자(부하 클라 등)를 위해 그대로 남겨 둔다.
    size_t ConsumeSubmitted(size_t size)
    {
        _lock.lock();

        if (size == 0 || _buffer == nullptr)
        {
            _lock.unlock();
            return 0;
        }

        if (GetSubmittedSize_Internal() < size)
        {
            _lock.unlock();
            return 0;
        }

        _readPos = (_readPos + size) % _capacity;

        _lock.unlock();
        return size;
    }

    size_t GetUnsubmittedSize()
    {
        _lock.lock();
        size_t result = GetUnsubmittedSize_Internal();
        _lock.unlock();
        return result;
    }

    // 제출했지만 아직 완료되지 않은 양 — 깊이 1에서는 곧 "이번 제출의 크기"다(부분 송신 판정용).
    size_t GetSubmittedSize()
    {
        _lock.lock();
        size_t result = GetSubmittedSize_Internal();
        _lock.unlock();
        return result;
    }

    // 제출 경계를 읽기 위치로 되돌린다 — 부분 송신이 남긴 잔여를 다시 "미제출"로 만들어 재전송시킨다.
    //   제출 경계가 없던 1-pending 시절에는 readPos만 밀면 잔여가 자동으로 재전송 대상이 됐다.
    //   그 결과를 그대로 재현하는 자리다.
    //   [주의] in-flight가 0인 시점에서만 호출할 것 — 여러 제출이 떠 있으면 어느 구간이
    //          남았는지 알 수 없어 아직 나가는 중인 바이트를 중복 전송하게 된다.
    size_t RewindSubmitted()
    {
        _lock.lock();
        const size_t rewound = GetSubmittedSize_Internal();
        _submitPos = _readPos;
        _lock.unlock();
        return rewound;
    }

public:
    char* _buffer;
    size_t _capacity;
    size_t _readPos;
    size_t _writePos;
    // [다중 pending 송신] 제출 경계 — read ≤ submit ≤ write.
    //   이 값을 쓰지 않는 사용자(수신 링·부하 클라)에게는 0에 머물러 무영향이다.
    size_t _submitPos;
    bool _ownsBuffer;   // false = 외부 소유 버퍼(InitExternal) — 소멸 시 delete 금지
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

    // 제출됐지만 아직 완료되지 않은 양 (submit - read)
    size_t GetSubmittedSize_Internal() const
    {
        if (_submitPos >= _readPos)
            return _submitPos - _readPos;
        else
            return _capacity - _readPos + _submitPos;
    }

    // 아직 제출하지 않은 양 (write - submit)
    size_t GetUnsubmittedSize_Internal() const
    {
        if (_writePos >= _submitPos)
            return _writePos - _submitPos;
        else
            return _capacity - _submitPos + _writePos;
    }

    CRingBufferT(const CRingBufferT&) = delete;
    CRingBufferT& operator=(const CRingBufferT&) = delete;
};

// === Type Aliases (사용 편의성) ===
using CRingBufferST = CRingBufferT<NoLock>;       // 싱글스레드 버전
using CRingBufferMT = CRingBufferT<MutexLock>;    // 멀티스레드 버전
