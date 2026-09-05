//
#include <iostream>
#include <vector>
#include <atomic>
#include <chrono>
#include <random>
#include <cstring>
#include <thread>
#include <string>
#include <memory>
#include <mutex>
#include <condition_variable>
//=============================================================================
// 테스트 대상 선택
//
// 두 헤더를 모두 포함한다. 수정본은 namespace Fixed 안에 있어 충돌하지 않는다.
//   ::CSerialBuffer        — 원본 사본
//   Fixed::CSerialBuffer   — 수정본
//
// 테스트 14종은 UTBuffer 별칭이 가리키는 쪽을 돌린다:
//   기본                      → 원본 사본
//   -DUSE_FIXED_SERIALBUFFER  → 수정본
//
// Phase 5(동등성)만은 별칭과 무관하게 두 구현을 직접 맞대어 비교한다.
//=============================================================================
#include "SerialBuffer.h"
#include "SerialBuffer_Fixed.h"

#ifdef USE_FIXED_SERIALBUFFER
using UTBuffer = Fixed::CSerialBuffer;
#else
using UTBuffer = ::CSerialBuffer;
#endif

// 두 구현의 버퍼 기하는 같아야 한다 — 다르면 동등성 비교 자체가 성립하지 않는다
static_assert(HEADER_SIZE == Fixed::HEADER_SIZE, "HEADER_SIZE 불일치");
static_assert(MSG_DEFAULT_SIZE == Fixed::MSG_DEFAULT_SIZE, "MSG_DEFAULT_SIZE 불일치");

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
// system("cls") 대체 — 프로세스 스폰 없이 콘솔 클리어 (경합 테스트 CPU 노이즈 제거)
static void ClearScreen()
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(h, &csbi)) return;
    DWORD cells = csbi.dwSize.X * csbi.dwSize.Y, written;
    COORD home = { 0, 0 };
    FillConsoleOutputCharacterA(h, ' ', cells, home, &written);
    FillConsoleOutputAttribute(h, csbi.wAttributes, cells, home, &written);
    SetConsoleCursorPosition(h, home);
}
#else
#include <cstdlib>
static void ClearScreen() { if (system("clear")) {} }
#endif

//=============================================================================
// 테스트 설정 상수 (반복 횟수 조절 가능)
//=============================================================================
namespace TestConfig
{
    // Phase 1: 직렬화 기본
    const uint64_t BASIC_TYPE_ITERATIONS = 3'000'000;       // 전 타입 왕복
    const uint64_t RANDOM_MIXED_ROUNDS = 3'000'000;         // 랜덤 혼합 타입 라운드
    const uint64_t STRING_ITERATIONS = 1'000'000;           // 문자열 왕복

    const uint64_t RECV_PATH_PACKETS = 2'000'000;           // recv 경로 통합 패킷 수

    // Phase 2: 수명 / 봉인
    const uint64_t LIFECYCLE_CYCLES = 1'000'000;            // Alloc→SubRef 사이클
    const int64_t  BATCH_ADDREF_TARGETS = 1'000;            // 배치 AddRef 타겟 수

    // Phase 3: 멀티스레드 / 실사용 소유권 경로
    const uint64_t MT_CYCLES_PER_THREAD = 300'000;          // 스레드당 Alloc/Free 사이클
    const uint64_t BROADCAST_ROUNDS = 200'000;              // 브로드캐스트 라운드
    const uint64_t SEND_FAILURE_ROUNDS = 200'000;           // 전송 실패 혼합 라운드
    const uint64_t ZERO_TARGET_ROUNDS = 500'000;            // 타겟 0명 브로드캐스트 라운드
    const uint64_t MIXED_ADDREF_ROUNDS = 200'000;           // 단건+배치 AddRef 혼합 라운드

    // Phase 5: 원본 ↔ 수정본 동등성
    const uint64_t EQUIVALENCE_ROUNDS = 2'000'000;          // 동등성 비교 라운드

    // 진행 상황 출력 주기
    const uint64_t PROGRESS_INTERVAL = 500'000;
}

// 전역 카운터
std::atomic<uint64_t> g_testCount(0);
std::atomic<uint64_t> g_totalIterations(0);
std::atomic<uint64_t> g_warnCount(0);

// 크래시 함수
void Crash()
{
    int* crash = nullptr;
    *crash = 0xDEADBEEF;
}

// 테스트 실패 시 크래시
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cout << "\n[CRASH] " << message << std::endl; \
            std::cout << "  File: " << __FILE__ << std::endl; \
            std::cout << "  Line: " << __LINE__ << std::endl; \
            std::cout << "  Iteration: " << g_totalIterations << std::endl; \
            Crash(); \
        } \
    } while(0)

// 잠재 결함 점검용 — 죽이지 않고 경고만 남긴다 (Phase 4 전용)
#define TEST_WARN(condition, message) \
    do { \
        if (!(condition)) { \
            std::cout << "  [WARN] " << message << "  (line " << __LINE__ << ")" << std::endl; \
            g_warnCount++; \
        } \
    } while(0)

// 진행 상황 출력
void PrintProgress(const char* testName, uint64_t current, uint64_t total)
{
    if (current % TestConfig::PROGRESS_INTERVAL == 0)
    {
        double progress = (double)current / total * 100.0;
        std::cout << "[" << testName << "] "
            << "진행: " << current << " / " << total
            << " (" << progress << "%)" << std::endl;
    }
}

// 테스트용 프리리스트 (LockFreeConfig.h 대체본) 접근
LockFree::CExternalTlsFreeList<UTBuffer>* Pool()
{
    return UTBuffer::_TlsMsgFreeList;
}

uint8_t MakeChecksum(const char* data, int size)
{
    uint32_t sum = 0;
    for (int i = 0; i < size; ++i)
        sum += static_cast<uint8_t>(data[i]);

    return static_cast<uint8_t>(sum & 0xFF);
}

//=============================================================================
// Phase 1-1: 전 타입 직렬화/역직렬화 왕복
// operator<< / operator>> 오버로드가 넣은 순서·값 그대로 회수하는지 검증
//=============================================================================
void Test_BasicTypes()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 1-1] 전 타입 왕복 테스트 시작" << std::endl;
    std::cout << "  목표: " << TestConfig::BASIC_TYPE_ITERATIONS << " 번 반복" << std::endl;
    std::cout << "========================================" << std::endl;

    UTBuffer buffer(MSG_DEFAULT_SIZE);
    std::mt19937_64 gen(0xC0FFEE);

    const int EXPECTED_SIZE =
        (int)(sizeof(uint8_t) + sizeof(char) + sizeof(short) + sizeof(uint16_t)
            + sizeof(int) + sizeof(uint32_t) + sizeof(int64_t) + sizeof(uint64_t)
            + sizeof(float) + sizeof(double));

    for (uint64_t i = 1; i <= TestConfig::BASIC_TYPE_ITERATIONS; ++i)
    {
        buffer.Clear();

        const uint8_t  inByte = static_cast<uint8_t>(gen());
        const char     inChar = static_cast<char>(gen());
        const short    inShort = static_cast<short>(gen());
        const uint16_t inUShort = static_cast<uint16_t>(gen());
        const int      inInt = static_cast<int>(gen());
        const uint32_t inUInt = static_cast<uint32_t>(gen());
        const int64_t  inInt64 = static_cast<int64_t>(gen());
        const uint64_t inUInt64 = gen();
        const float    inFloat = static_cast<float>(gen() % 100000) / 7.0f;
        const double   inDouble = static_cast<double>(gen() % 1000000) / 13.0;

        buffer << inByte << inChar << inShort << inUShort << inInt
            << inUInt << inInt64 << inUInt64 << inFloat << inDouble;

        TEST_ASSERT(buffer.GetDataSize() == EXPECTED_SIZE, "직렬화 후 DataSize 불일치");

        uint8_t  outByte = 0;
        char     outChar = 0;
        short    outShort = 0;
        uint16_t outUShort = 0;
        int      outInt = 0;
        uint32_t outUInt = 0;
        int64_t  outInt64 = 0;
        uint64_t outUInt64 = 0;
        float    outFloat = 0.0f;
        double   outDouble = 0.0;

        buffer >> outByte >> outChar >> outShort >> outUShort >> outInt
            >> outUInt >> outInt64 >> outUInt64 >> outFloat >> outDouble;

        TEST_ASSERT(outByte == inByte, "uint8_t 값 불일치");
        TEST_ASSERT(outChar == inChar, "char 값 불일치");
        TEST_ASSERT(outShort == inShort, "short 값 불일치");
        TEST_ASSERT(outUShort == inUShort, "uint16_t 값 불일치");
        TEST_ASSERT(outInt == inInt, "int 값 불일치");
        TEST_ASSERT(outUInt == inUInt, "uint32_t 값 불일치");
        TEST_ASSERT(outInt64 == inInt64, "int64_t 값 불일치");
        TEST_ASSERT(outUInt64 == inUInt64, "uint64_t 값 불일치");
        TEST_ASSERT(std::memcmp(&outFloat, &inFloat, sizeof(float)) == 0, "float 비트 불일치");
        TEST_ASSERT(std::memcmp(&outDouble, &inDouble, sizeof(double)) == 0, "double 비트 불일치");
        TEST_ASSERT(buffer.GetDataSize() == 0, "역직렬화 후 잔여 데이터 존재");

        g_totalIterations++;
        PrintProgress("전 타입", i, TestConfig::BASIC_TYPE_ITERATIONS);
    }

    std::cout << "\n[PASS] 전 타입 왕복 테스트 완료!" << std::endl;
    g_testCount++;
}

//=============================================================================
// Phase 1-2: 랜덤 혼합 타입 대량 왕복
// 타입/개수를 매 라운드 무작위로 섞어 순서 보존과 값 손상 여부 검증
//=============================================================================
void Test_RandomMixed()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 1-2] 랜덤 혼합 타입 왕복 테스트 시작" << std::endl;
    std::cout << "  목표: " << TestConfig::RANDOM_MIXED_ROUNDS << " 라운드" << std::endl;
    std::cout << "========================================" << std::endl;

    UTBuffer buffer(MSG_DEFAULT_SIZE);
    std::mt19937_64 gen(0xBADC0DE);
    std::uniform_int_distribution<int> fieldCountDis(1, 100);
    std::uniform_int_distribution<int> typeDis(0, 5);

    struct Field
    {
        int type;
        uint64_t raw;
    };

    std::vector<Field> fields;
    fields.reserve(100);

    for (uint64_t round = 1; round <= TestConfig::RANDOM_MIXED_ROUNDS; ++round)
    {
        buffer.Clear();
        fields.clear();

        const int fieldCount = fieldCountDis(gen);
        int expectedSize = 0;

        for (int f = 0; f < fieldCount; ++f)
        {
            Field field;
            field.type = typeDis(gen);
            field.raw = gen();

            switch (field.type)
            {
            case 0: buffer << static_cast<uint8_t>(field.raw);  expectedSize += 1; break;
            case 1: buffer << static_cast<char>(field.raw);     expectedSize += 1; break;
            case 2: buffer << static_cast<short>(field.raw);    expectedSize += 2; break;
            case 3: buffer << static_cast<uint16_t>(field.raw); expectedSize += 2; break;
            case 4: buffer << static_cast<int>(field.raw);      expectedSize += 4; break;
            default: buffer << static_cast<uint64_t>(field.raw); expectedSize += 8; break;
            }

            fields.push_back(field);
        }

        TEST_ASSERT(buffer.GetDataSize() == expectedSize, "혼합 직렬화 DataSize 불일치");

        for (size_t f = 0; f < fields.size(); ++f)
        {
            const Field& field = fields[f];

            switch (field.type)
            {
            case 0:
            {
                uint8_t value = 0;
                buffer >> value;
                TEST_ASSERT(value == static_cast<uint8_t>(field.raw), "uint8_t 값 손상");
                break;
            }
            case 1:
            {
                char value = 0;
                buffer >> value;
                TEST_ASSERT(value == static_cast<char>(field.raw), "char 값 손상");
                break;
            }
            case 2:
            {
                short value = 0;
                buffer >> value;
                TEST_ASSERT(value == static_cast<short>(field.raw), "short 값 손상");
                break;
            }
            case 3:
            {
                uint16_t value = 0;
                buffer >> value;
                TEST_ASSERT(value == static_cast<uint16_t>(field.raw), "uint16_t 값 손상");
                break;
            }
            case 4:
            {
                int value = 0;
                buffer >> value;
                TEST_ASSERT(value == static_cast<int>(field.raw), "int 값 손상");
                break;
            }
            default:
            {
                uint64_t value = 0;
                buffer >> value;
                TEST_ASSERT(value == field.raw, "uint64_t 값 손상");
                break;
            }
            }
        }

        TEST_ASSERT(buffer.GetDataSize() == 0, "혼합 역직렬화 후 잔여 데이터 존재");

        g_totalIterations++;
        PrintProgress("랜덤 혼합", round, TestConfig::RANDOM_MIXED_ROUNDS);
    }

    std::cout << "\n[PASS] 랜덤 혼합 타입 왕복 테스트 완료!" << std::endl;
    g_testCount++;
}

//=============================================================================
// Phase 1-3: 문자열 직렬화
// operator<<(const char*) / (const wchar_t*) 는 [short 길이][본문] 으로 기록한다.
// 대응하는 operator>> 가 없으므로 수신측은 길이를 읽고 GetData 로 본문을 가져간다.
//=============================================================================
void Test_String()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 1-3] 문자열 직렬화 테스트 시작" << std::endl;
    std::cout << "  목표: " << TestConfig::STRING_ITERATIONS << " 번 반복" << std::endl;
    std::cout << "========================================" << std::endl;

    UTBuffer buffer(MSG_DEFAULT_SIZE);
    std::mt19937_64 gen(0x5EED5EED);
    std::uniform_int_distribution<int> lenDis(0, 200);

    char narrowOut[256];
    wchar_t wideOut[256];

    for (uint64_t i = 1; i <= TestConfig::STRING_ITERATIONS; ++i)
    {
        buffer.Clear();

        const int length = lenDis(gen);
        std::string inNarrow;
        inNarrow.reserve(length);
        for (int c = 0; c < length; ++c)
            inNarrow.push_back(static_cast<char>('a' + (gen() % 26)));

        std::wstring inWide(inNarrow.begin(), inNarrow.end());

        buffer << inNarrow.c_str() << inWide.c_str();

        const int expectedSize = (int)(sizeof(short) + inNarrow.size()
            + sizeof(short) + inWide.size() * sizeof(wchar_t));
        TEST_ASSERT(buffer.GetDataSize() == expectedSize, "문자열 직렬화 DataSize 불일치");

        // --- narrow ---
        short narrowLen = -1;
        buffer >> narrowLen;
        TEST_ASSERT(narrowLen == (short)inNarrow.size(), "narrow 길이 접두사 불일치");
        if (narrowLen > 0)
        {
            TEST_ASSERT(buffer.GetData(narrowOut, narrowLen) == narrowLen, "narrow 본문 읽기 실패");
            TEST_ASSERT(std::memcmp(narrowOut, inNarrow.data(), narrowLen) == 0, "narrow 본문 손상");
        }

        // --- wide (길이는 바이트 수) ---
        short wideBytes = -1;
        buffer >> wideBytes;
        TEST_ASSERT(wideBytes == (short)(inWide.size() * sizeof(wchar_t)), "wide 길이 접두사 불일치");
        if (wideBytes > 0)
        {
            TEST_ASSERT(buffer.GetData((char*)wideOut, wideBytes) == wideBytes, "wide 본문 읽기 실패");
            TEST_ASSERT(std::memcmp(wideOut, inWide.data(), wideBytes) == 0, "wide 본문 손상");
        }

        TEST_ASSERT(buffer.GetDataSize() == 0, "문자열 역직렬화 후 잔여 데이터 존재");

        g_totalIterations++;
        PrintProgress("문자열", i, TestConfig::STRING_ITERATIONS);
    }

    std::cout << "\n[PASS] 문자열 직렬화 테스트 완료!" << std::endl;
    g_testCount++;
}

//=============================================================================
// Phase 1-4: 경계 조건
// IsFull/IsEmpty, 정확히 가득 채우기, MoveWritePos/MoveReadPos, PeekData
//=============================================================================
void Test_Boundary()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 1-4] 경계 조건 테스트 시작" << std::endl;
    std::cout << "========================================" << std::endl;

    // 헤더 2바이트를 뺀 실사용 공간이 4의 배수가 되도록 잡는다 (int 로 정확히 채우기 위함)
    const int BUFFER_SIZE = 130;
    const int USABLE = BUFFER_SIZE - HEADER_SIZE;   // 128

    // --- 1. 정확히 가득 채우기 ---
    {
        UTBuffer buffer(BUFFER_SIZE);
        TEST_ASSERT(buffer.GetBufferSize() == BUFFER_SIZE, "GetBufferSize 불일치");
        TEST_ASSERT(buffer.GetDataSize() == 0, "초기 DataSize 불일치");
        TEST_ASSERT(buffer.IsFull(USABLE) == false, "정확히 맞는 크기를 가득으로 판정");
        TEST_ASSERT(buffer.IsFull(USABLE + 1), "1바이트 초과를 허용으로 판정");

        for (int i = 0; i < USABLE / 4; ++i)
            buffer << (int)(i * 0x01010101);

        TEST_ASSERT(buffer.GetDataSize() == (USABLE / 4) * 4, "가득 채운 뒤 DataSize 불일치");
        TEST_ASSERT(buffer.IsFull(1), "가득 찬 버퍼가 여유 있다고 판정");

        // 넘치는 쓰기는 무시되어야 한다 (원본은 실패해도 반환값이 없음 → DataSize 로 확인)
        const int before = buffer.GetDataSize();
        buffer << (int)0xDEADBEEF;
        TEST_ASSERT(buffer.GetDataSize() == before, "가득 찬 버퍼에 쓰기가 반영됨");

        uint8_t overflow = 0xFF;
        TEST_ASSERT(buffer.SetData((char*)&overflow, 1) == 0, "가득 찬 버퍼에 SetData 가 성공함");

        for (int i = 0; i < USABLE / 4; ++i)
        {
            int value = 0;
            buffer >> value;
            TEST_ASSERT(value == (int)(i * 0x01010101), "가득 찬 버퍼 역직렬화 값 손상");
        }
        TEST_ASSERT(buffer.GetDataSize() == 0, "소진 후 DataSize 불일치");
    }
    std::cout << "  [OK] 정확히 가득 채우기 / 초과 쓰기 거부" << std::endl;

    // --- 2. 언더플로우 ---
    {
        UTBuffer buffer(BUFFER_SIZE);
        buffer << (int)0x11223344;

        TEST_ASSERT(buffer.IsEmpty(5), "데이터보다 큰 요청을 읽기 가능으로 판정");
        TEST_ASSERT(buffer.IsEmpty(4) == false, "데이터와 같은 크기를 비었다고 판정");

        char dest[8];
        TEST_ASSERT(buffer.GetData(dest, 8) == 0, "부족한 데이터 읽기가 성공함");
        TEST_ASSERT(buffer.GetDataSize() == 4, "실패한 읽기가 DataSize 를 변경함");

        // 원본 계약: 읽을 게 없으면 값을 0으로 채우고 위치는 그대로 둔다
        int64_t tooBig = -1;
        buffer >> tooBig;
        TEST_ASSERT(tooBig == 0, "언더플로우 시 값이 0으로 초기화되지 않음");
        TEST_ASSERT(buffer.GetDataSize() == 4, "언더플로우가 읽기 위치를 이동시킴");

        int value = 0;
        buffer >> value;
        TEST_ASSERT(value == 0x11223344, "언더플로우 시도 후 정상 값이 손상됨");
    }
    std::cout << "  [OK] 언더플로우 방어 (값 0 초기화 / 위치 불변)" << std::endl;

    // --- 3. PeekData 는 읽기 위치를 옮기지 않는다 ---
    {
        UTBuffer buffer(BUFFER_SIZE);
        buffer << (uint32_t)0xDEADBEEF;

        uint32_t peeked = 0;
        TEST_ASSERT(buffer.PeekData((char*)&peeked, 4) == 4, "Peek 실패");
        TEST_ASSERT(peeked == 0xDEADBEEF, "Peek 값 불일치");
        TEST_ASSERT(buffer.GetDataSize() == 4, "Peek 이 읽기 위치를 이동시킴");

        uint32_t read = 0;
        buffer >> read;
        TEST_ASSERT(read == 0xDEADBEEF, "Peek 이후 읽은 값 불일치");
    }
    std::cout << "  [OK] PeekData 비파괴 읽기" << std::endl;

    // --- 4. MoveWritePos / MoveReadPos ---
    {
        UTBuffer buffer(BUFFER_SIZE);

        // recv 가 직접 채운 상황 모사
        const char source[16] = "ZeroCopyWrite!";
        std::memcpy(buffer.GetWriteBufferPtr(), source, sizeof(source));
        TEST_ASSERT(buffer.MoveWritePos(sizeof(source)) == (int)sizeof(source), "쓰기 위치 이동 실패");
        TEST_ASSERT(buffer.GetDataSize() == (int)sizeof(source), "쓰기 이동 후 DataSize 불일치");
        TEST_ASSERT(std::memcmp(buffer.GetReadBufferPtr(), source, sizeof(source)) == 0, "직접 기록 데이터 손상");

        TEST_ASSERT(buffer.MoveWritePos(USABLE) == 0, "여유 공간 초과 쓰기 이동이 성공함");

        // 원본 계약: 가진 데이터보다 많이 읽으려 하면 가진 만큼만 이동하고 그 값을 반환
        const int moved = buffer.MoveReadPos((int)sizeof(source) + 100);
        TEST_ASSERT(moved == (int)sizeof(source), "초과 읽기 이동 반환값이 보유 데이터와 불일치");
        TEST_ASSERT(buffer.GetDataSize() == 0, "초과 읽기 이동 후 DataSize 가 0이 아님");
    }
    std::cout << "  [OK] MoveWritePos / MoveReadPos (초과 시 클램프)" << std::endl;

    // --- 5. 헤더 영역과 페이로드 영역의 분리 ---
    {
        UTBuffer buffer(BUFFER_SIZE);
        buffer << (uint64_t)0x1122334455667788ULL;

        TEST_ASSERT(buffer.GetPayloadBufferPtr() == buffer.GetHeaderBufferPtr() + HEADER_SIZE,
            "페이로드 시작이 헤더 크기만큼 밀려있지 않음");
        TEST_ASSERT(buffer.GetReadBufferPtr() == buffer.GetPayloadBufferPtr(),
            "읽기 포인터 초기 위치 불일치");
        TEST_ASSERT(buffer.GetWriteBufferPtr() == buffer.GetPayloadBufferPtr() + 8,
            "쓰기 포인터가 기록한 만큼 이동하지 않음");

        // 헤더에 길이를 써도 페이로드가 침범당하지 않아야 한다
        const short payloadLen = (short)buffer.GetDataSize();
        std::memcpy(buffer.GetHeaderBufferPtr(), &payloadLen, HEADER_SIZE);

        uint64_t value = 0;
        buffer >> value;
        TEST_ASSERT(value == 0x1122334455667788ULL, "헤더 기록이 페이로드를 침범함");

        short readLen = 0;
        std::memcpy(&readLen, buffer.GetHeaderBufferPtr(), HEADER_SIZE);
        TEST_ASSERT(readLen == payloadLen, "헤더 값이 보존되지 않음");
    }
    std::cout << "  [OK] 헤더 2바이트 예약 영역 분리" << std::endl;

    std::cout << "\n[PASS] 경계 조건 테스트 완료!" << std::endl;
    g_testCount++;
}

//=============================================================================
// Phase 1-5: 잘못된 인자 방어 (C2)
//
// size=0 과 음수 크기를 각 API 에 넘겼을 때 어떻게 되는지 본다.
//
// 주의: SetData/GetData 에 음수 크기나 nullptr 을 넘기는 경로는 실행하지 않는다.
//   둘 다 memcpy_s 까지 내려가는데, MSVC 의 memcpy_s 는 잘못된 인자에서
//   invalid parameter handler 를 부르고 기본 설정이면 프로세스를 죽인다.
//   즉 "테스트가 서버를 죽이는" 형태가 되므로 Phase 4 에서 계산으로만 확인한다.
//=============================================================================
void Test_InvalidArguments()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 1-5] 잘못된 인자 방어 테스트 시작" << std::endl;
    std::cout << "========================================" << std::endl;

    // --- 1. size = 0 은 무해해야 한다 ---
    {
        UTBuffer buffer(128);
        int payload = 0x11223344;
        buffer << payload;

        const int dataBefore = buffer.GetDataSize();
        char scratch[8];

        TEST_ASSERT(buffer.SetData((char*)&payload, 0) == 0, "size=0 SetData 가 0 이외를 반환");
        TEST_ASSERT(buffer.GetData(scratch, 0) == 0, "size=0 GetData 가 0 이외를 반환");
        TEST_ASSERT(buffer.PeekData(scratch, 0) == 0, "size=0 PeekData 가 0 이외를 반환");
        TEST_ASSERT(buffer.MoveWritePos(0) == 0, "size=0 MoveWritePos 가 0 이외를 반환");
        TEST_ASSERT(buffer.MoveReadPos(0) == 0, "size=0 MoveReadPos 가 0 이외를 반환");
        TEST_ASSERT(buffer.GetDataSize() == dataBefore, "size=0 호출이 버퍼 상태를 변경함");

        int readBack = 0;
        buffer >> readBack;
        TEST_ASSERT(readBack == payload, "size=0 호출 이후 데이터가 손상됨");
    }
    std::cout << "  [OK] size=0 은 상태를 바꾸지 않고 0 을 반환" << std::endl;

    // --- 2. MoveWritePos 에 음수 ---
    //   IsFull(-1) 이 false 라 가드를 통과하고 _rear 가 뒤로 밀린다.
    //   상태가 오염되므로 이 버퍼는 이 블록 밖에서 쓰지 않는다.
    {
        UTBuffer buffer(128);
        TEST_ASSERT(buffer.GetDataSize() == 0, "초기 상태 불일치");

        const int moved = buffer.MoveWritePos(-1);
        const int dataSize = buffer.GetDataSize();

        std::cout << "  MoveWritePos(-1) 반환값                 : " << moved << std::endl;
        std::cout << "  그 뒤 GetDataSize()                     : " << dataSize << std::endl;

        TEST_ASSERT(moved != 0 || dataSize == 0,
            "음수 이동이 거부되지도, 상태를 바꾸지도 않는 모순 상태");
        // 이 버퍼는 여기서 버린다 (쓰기 위치가 페이로드 앞쪽으로 넘어간 상태)
    }

    // --- 3. MoveReadPos 에 음수 ---
    //   _front + size > _rear 비교가 음수에서 성립하지 않아 else 분기로 빠지고,
    //   _DataSize -= (음수) 가 되어 없던 데이터가 생긴다.
    {
        UTBuffer buffer(128);
        const int moved = buffer.MoveReadPos(-1);
        const int dataSize = buffer.GetDataSize();

        std::cout << "  MoveReadPos(-1) 반환값                  : " << moved << std::endl;
        std::cout << "  그 뒤 GetDataSize()                     : " << dataSize << std::endl;

        TEST_ASSERT(moved != 0 || dataSize == 0,
            "음수 이동이 거부되지도, 상태를 바꾸지도 않는 모순 상태");
    }
    std::cout << "  [OK] 음수 이동의 실제 동작 기록 (Phase 4 에서 위험도 평가)" << std::endl;

    // --- 4. IsEmpty / IsFull 의 0 과 음수 ---
    {
        UTBuffer buffer(128);
        buffer << (int)0;

        // 0 바이트 요청은 언제나 가능해야 한다 — 이건 계약이다
        TEST_ASSERT(buffer.IsEmpty(0) == false, "IsEmpty(0) 이 true");
        TEST_ASSERT(buffer.IsFull(0) == false, "빈 버퍼에서 IsFull(0) 이 true");

        // 음수 입력의 반환값은 계약이 아니라 관찰 대상이다.
        //   거부(true)가 옳은 동작이고, 통과(false)면 위 2·3번처럼 상태가 망가진다.
        //   어느 쪽이든 어서션으로 고정하지 않는다 — 고정하면 나중에 가드를 넣을 때
        //   테스트가 수정을 막는다 (B2 에서 겪은 함정).
        std::cout << "  IsEmpty(-1) / IsFull(-1)                : "
            << (buffer.IsEmpty(-1) ? "거부" : "통과") << " / "
            << (buffer.IsFull(-1) ? "거부" : "통과") << std::endl;
    }
    std::cout << "  [OK] IsEmpty/IsFull 의 0·음수 입력 동작 기록" << std::endl;

    std::cout << "\n[PASS] 잘못된 인자 방어 테스트 완료!" << std::endl;
    g_testCount++;
}

//=============================================================================
// Phase 1-6: recv 경로 통합 (C3)
//
// CIOCPServer::ParsePackets 를 그대로 모사한다:
//   1) 링버퍼에 헤더 크기만큼 있는지 확인
//   2) 첫 2바이트(size) peek
//   3) 전체 패킷 크기 계산 및 검증 (headerSize ≤ total ≤ MAX_PACKET_SIZE)
//   4) 전체가 도착했는지 확인 (부족하면 다음 recv 대기 — 부분 수신)
//   5) Alloc → GetWriteBufferPtr() 에 직접 적재 → MoveWritePos
//   6) 컨텐츠가 operator>> 로 헤더와 필드를 파싱
//
// 기존 경계 테스트는 MoveWritePos 로 채운 뒤 memcmp 로 바이트만 비교했다.
// 여기서는 실제처럼 operator>> 파싱까지 연결해서 본다.
//=============================================================================
namespace RecvPath
{
    // 실제 MsgHeader 와 같은 모양 (size = 헤더 포함 전체 크기, type)
#pragma pack(push, 1)
    struct MsgHeader
    {
        uint16_t size;
        uint16_t type;
    };
#pragma pack(pop)

    const int HEADER_BYTES = (int)sizeof(MsgHeader);
    const int MAX_PACKET = MSG_DEFAULT_SIZE - HEADER_SIZE;   // 1458 — 실제 MAX_PACKET_SIZE 와 동일
}

void Test_RecvPathIntegration()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 1-6] recv 경로 통합 테스트 시작" << std::endl;
    std::cout << "  목표: " << TestConfig::RECV_PATH_PACKETS << " 패킷" << std::endl;
    std::cout << "========================================" << std::endl;

    std::mt19937 gen(0x2ECFU);
    std::uniform_int_distribution<int> fieldDis(1, 40);
    std::uniform_int_distribution<int> chunkDis(1, 200);   // 소켓이 한 번에 주는 바이트 수

    std::vector<char> wire;        // 송신측이 만들어 둔 바이트 (아직 도착 전)
    std::vector<char> stream;      // 수신 링버퍼에 쌓인 바이트
    wire.reserve(8192);
    stream.reserve(8192);

    uint64_t parsedPackets = 0;
    uint64_t partialHeader = 0;    // 헤더도 다 안 온 경우
    uint64_t partialBody = 0;      // 헤더는 왔는데 본문이 덜 온 경우
    uint64_t sequence = 0;

    while (parsedPackets < TestConfig::RECV_PATH_PACKETS)
    {
        // --- 송신측: 패킷을 만들어 wire 에 쌓는다 ---
        while (wire.size() < 2048)
        {
            const int fieldCount = fieldDis(gen);
            const int payloadBytes = (int)(sizeof(uint64_t) + fieldCount * sizeof(int));
            const int totalSize = RecvPath::HEADER_BYTES + payloadBytes;
            TEST_ASSERT(totalSize <= RecvPath::MAX_PACKET, "생성 패킷이 최대 크기를 넘음");

            RecvPath::MsgHeader header;
            header.size = (uint16_t)totalSize;
            header.type = (uint16_t)fieldCount;

            const size_t base = wire.size();
            wire.resize(base + totalSize);
            char* out = wire.data() + base;

            std::memcpy(out, &header, RecvPath::HEADER_BYTES);
            out += RecvPath::HEADER_BYTES;

            const uint64_t seq = sequence++;
            std::memcpy(out, &seq, sizeof(seq));
            out += sizeof(seq);
            for (int f = 0; f < fieldCount; ++f)
            {
                const int value = (int)(seq * 31 + f);
                std::memcpy(out, &value, sizeof(value));
                out += sizeof(value);
            }
        }

        // --- 소켓: 임의 크기로 잘라서 전달한다 (패킷 경계와 무관) ---
        const size_t chunk = (std::min)((size_t)chunkDis(gen), wire.size());
        stream.insert(stream.end(), wire.begin(), wire.begin() + chunk);
        wire.erase(wire.begin(), wire.begin() + chunk);

        // --- 여기부터 ParsePackets 모사 ---
        size_t consumed = 0;
        while (parsedPackets < TestConfig::RECV_PATH_PACKETS)
        {
            const size_t dataSize = stream.size() - consumed;

            // 1. 헤더 크기 체크
            if (dataSize < (size_t)RecvPath::HEADER_BYTES)
            {
                ++partialHeader;
                break;
            }

            // 2. 첫 2바이트(size) peek
            uint16_t packetSize = 0;
            std::memcpy(&packetSize, stream.data() + consumed, sizeof(uint16_t));

            // 3~4. 크기 검증
            const size_t totalPacketSize = packetSize;
            TEST_ASSERT(totalPacketSize >= (size_t)RecvPath::HEADER_BYTES, "패킷 크기가 헤더보다 작음");
            TEST_ASSERT(totalPacketSize <= (size_t)RecvPath::MAX_PACKET, "패킷 크기가 최대치 초과");

            // 5. 전체 도착 확인 — 본문이 덜 왔으면 다음 recv 대기
            if (dataSize < totalPacketSize)
            {
                ++partialBody;
                break;
            }

            // 6. Alloc → GetWriteBufferPtr() 에 직접 적재 → MoveWritePos (실제 코드와 동일)
            UTBuffer* pMsg = UTBuffer::Alloc();
            std::memcpy(pMsg->GetWriteBufferPtr(), stream.data() + consumed, totalPacketSize);
            const int movedSize = pMsg->MoveWritePos((int)totalPacketSize);
            TEST_ASSERT(movedSize == (int)totalPacketSize, "MoveWritePos 실패 — 불변식 위반");
            TEST_ASSERT(pMsg->GetDataSize() == (int)totalPacketSize, "적재 후 DataSize 불일치");

            consumed += totalPacketSize;

            // 7. 컨텐츠 파싱 — 헤더도 페이로드 영역에 들어있으므로 함께 읽어낸다
            uint16_t outSize = 0, outType = 0;
            *pMsg >> outSize >> outType;
            TEST_ASSERT(outSize == packetSize, "역직렬화한 헤더 size 불일치");

            uint64_t outSeq = 0;
            *pMsg >> outSeq;

            const int fieldCount = (int)outType;
            for (int f = 0; f < fieldCount; ++f)
            {
                int value = 0;
                *pMsg >> value;
                TEST_ASSERT(value == (int)(outSeq * 31 + f), "역직렬화 필드 값 손상");
            }
            TEST_ASSERT(pMsg->GetDataSize() == 0, "패킷 소진 후 잔여 데이터 존재");

            pMsg->SubRef();

            ++parsedPackets;
            g_totalIterations++;
            PrintProgress("recv 경로", parsedPackets, TestConfig::RECV_PATH_PACKETS);
        }

        // 소비한 만큼 링버퍼에서 제거
        if (consumed > 0)
            stream.erase(stream.begin(), stream.begin() + consumed);
    }

    TEST_ASSERT(partialHeader > 0, "헤더 부분 수신이 한 번도 발생하지 않음 — 경로 미검증");
    TEST_ASSERT(partialBody > 0, "본문 부분 수신이 한 번도 발생하지 않음 — 경로 미검증");
    TEST_ASSERT(Pool()->GetLiveCount() == 0, "recv 경로 종료 후 누수된 버퍼 존재");

    std::cout << "\n[PASS] recv 경로 통합 테스트 완료!" << std::endl;
    std::cout << "  부분 수신 — 헤더 미완: " << partialHeader
        << " / 본문 미완: " << partialBody << std::endl;
    g_testCount++;
}

//=============================================================================
// Phase 2-1: Seal 이후 동작
// 봉인 뒤 쓰기/읽기는 막히고, 브로드캐스트 경로인 PeekData 만 열려 있다.
//=============================================================================
void Test_Seal()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 2-1] Seal 봉인 테스트 시작" << std::endl;
    std::cout << "========================================" << std::endl;

    UTBuffer buffer(128);
    TEST_ASSERT(buffer.IsSealed() == false, "초기 상태가 봉인됨");

    buffer << (int)0x11223344 << (int)0x55667788;
    const int sealedSize = buffer.GetDataSize();

    buffer.Seal();
    TEST_ASSERT(buffer.IsSealed(), "Seal 이후 IsSealed 가 false");

    // --- 쓰기 차단 ---
    buffer << (int)0x99AABBCC;
    TEST_ASSERT(buffer.GetDataSize() == sealedSize, "Seal 이후 operator<< 가 반영됨");

    int extra = 0x99AABBCC;
    TEST_ASSERT(buffer.SetData((char*)&extra, 4) == 0, "Seal 이후 SetData 가 성공함");
    TEST_ASSERT(buffer.GetDataSize() == sealedSize, "Seal 이후 SetData 가 DataSize 를 변경함");
    std::cout << "  [OK] Seal 이후 쓰기 차단 (operator<< / SetData)" << std::endl;

    // --- 읽기 차단: 값은 0으로 채워지고 위치는 그대로 ---
    int value = -1;
    buffer >> value;
    TEST_ASSERT(value == 0, "Seal 이후 operator>> 가 값을 반환함");
    TEST_ASSERT(buffer.GetDataSize() == sealedSize, "Seal 이후 operator>> 가 위치를 이동시킴");

    char dest[8];
    TEST_ASSERT(buffer.GetData(dest, 4) == 0, "Seal 이후 GetData 가 성공함");
    TEST_ASSERT(buffer.GetDataSize() == sealedSize, "Seal 이후 GetData 가 위치를 이동시킴");
    std::cout << "  [OK] Seal 이후 읽기 차단 (operator>> / GetData)" << std::endl;

    // --- PeekData 는 열려 있다: 봉인된 버퍼를 N개 스레드가 읽어가는 경로 ---
    int peeked = 0;
    TEST_ASSERT(buffer.PeekData((char*)&peeked, 4) == 4, "Seal 이후 PeekData 가 막힘");
    TEST_ASSERT(peeked == 0x11223344, "Seal 이후 PeekData 값 손상");
    TEST_ASSERT(buffer.GetDataSize() == sealedSize, "PeekData 가 위치를 이동시킴");
    std::cout << "  [OK] Seal 이후 PeekData 는 허용 (브로드캐스트 읽기 경로)" << std::endl;

    // --- Clear 로 봉인 해제 ---
    buffer.Clear();
    TEST_ASSERT(buffer.IsSealed() == false, "Clear 가 봉인을 해제하지 않음");
    TEST_ASSERT(buffer.GetDataSize() == 0, "Clear 후 DataSize 가 0이 아님");
    buffer << (int)0x12345678;
    TEST_ASSERT(buffer.GetDataSize() == 4, "봉인 해제 후 쓰기가 반영되지 않음");
    std::cout << "  [OK] Clear 를 통한 봉인 해제" << std::endl;

    std::cout << "\n[PASS] Seal 봉인 테스트 완료!" << std::endl;
    g_testCount++;
}

//=============================================================================
// Phase 2-2: 참조 카운팅 기본
// Alloc 은 RefCount=1(생성자 소유권)로 반환, 0이 될 때 정확히 1회만 Free
//=============================================================================
void Test_RefCountBasic()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 2-2] 참조 카운팅 기본 테스트 시작" << std::endl;
    std::cout << "========================================" << std::endl;

    // --- 1. Alloc 직후 소유권 1개 ---
    {
        Pool()->ResetCounters();
        UTBuffer* msg = UTBuffer::Alloc();

        TEST_ASSERT(Pool()->GetAllocCount() == 1, "Alloc 횟수 불일치");
        TEST_ASSERT(msg->_RefCount.load() == 1, "Alloc 반환 버퍼의 RefCount 가 1이 아님");
        TEST_ASSERT(msg->GetDataSize() == 0, "Alloc 반환 버퍼가 비어있지 않음");
        TEST_ASSERT(msg->IsSealed() == false, "Alloc 반환 버퍼가 봉인 상태");

        msg->SubRef();
        TEST_ASSERT(Pool()->GetFreeCount() == 1, "RefCount 0 에서 Free 가 호출되지 않음");
        TEST_ASSERT(Pool()->GetLiveCount() == 0, "회수 후 살아있는 버퍼가 남음");
    }
    std::cout << "  [OK] Alloc(RefCount=1) → SubRef → Free 1회" << std::endl;

    // --- 2. AddRef / SubRef 균형 ---
    {
        Pool()->ResetCounters();
        UTBuffer* msg = UTBuffer::Alloc();

        for (int i = 0; i < 100; ++i)
            msg->AddRef();

        TEST_ASSERT(msg->_RefCount.load() == 101, "AddRef 100회 후 RefCount 불일치");

        for (int i = 0; i < 100; ++i)
        {
            msg->SubRef();
            TEST_ASSERT(Pool()->GetFreeCount() == 0, "참조가 남았는데 Free 가 호출됨");
        }

        TEST_ASSERT(msg->_RefCount.load() == 1, "SubRef 100회 후 RefCount 불일치");
        msg->SubRef();
        TEST_ASSERT(Pool()->GetFreeCount() == 1, "마지막 SubRef 에서 Free 가 1회 호출되지 않음");
        TEST_ASSERT(Pool()->GetLiveCount() == 0, "회수 후 살아있는 버퍼가 남음");
    }
    std::cout << "  [OK] AddRef/SubRef 균형 — 참조가 남는 동안 회수 안 됨" << std::endl;

    // --- 3. 배치 AddRef (브로드캐스트 타겟 수만큼 1회) ---
    {
        Pool()->ResetCounters();
        UTBuffer* msg = UTBuffer::Alloc();

        const int64_t targets = TestConfig::BATCH_ADDREF_TARGETS;
        msg->AddRef(targets);
        TEST_ASSERT(msg->_RefCount.load() == targets + 1, "배치 AddRef 후 RefCount 불일치");

        // 전송 완료 콜백 N회
        for (int64_t i = 0; i < targets; ++i)
            msg->SubRef();

        TEST_ASSERT(Pool()->GetFreeCount() == 0, "타겟 SubRef 만으로 회수됨 (생성자 소유권 무시)");
        TEST_ASSERT(msg->_RefCount.load() == 1, "타겟 SubRef 후 생성자 소유권이 남지 않음");

        msg->SubRef();
        TEST_ASSERT(Pool()->GetFreeCount() == 1, "생성자 소유권 반납 시 Free 가 1회 호출되지 않음");
        TEST_ASSERT(Pool()->GetLiveCount() == 0, "회수 후 살아있는 버퍼가 남음");
    }
    std::cout << "  [OK] 배치 AddRef(N) + SubRef×N + 소유권 반납 → Free 1회" << std::endl;

    // --- 4. 대량 Alloc/회수 사이클 — 누수 / 이중 해제 없음 ---
    {
        Pool()->ResetCounters();

        for (uint64_t i = 1; i <= TestConfig::LIFECYCLE_CYCLES; ++i)
        {
            UTBuffer* msg = UTBuffer::Alloc();
            msg->AddRef(3);
            *msg << (uint64_t)i;
            msg->Seal();

            uint64_t peeked = 0;
            TEST_ASSERT(msg->PeekData((char*)&peeked, sizeof(peeked)) == (int)sizeof(peeked),
                "봉인 버퍼 PeekData 실패");
            TEST_ASSERT(peeked == i, "사이클 중 데이터 손상");

            msg->SubRef();
            msg->SubRef();
            msg->SubRef();
            msg->SubRef();   // 생성자 소유권

            g_totalIterations++;
            PrintProgress("수명 사이클", i, TestConfig::LIFECYCLE_CYCLES);
        }

        TEST_ASSERT(Pool()->GetAllocCount() == (int64_t)TestConfig::LIFECYCLE_CYCLES, "Alloc 총량 불일치");
        TEST_ASSERT(Pool()->GetFreeCount() == (int64_t)TestConfig::LIFECYCLE_CYCLES, "Free 총량 불일치");
        TEST_ASSERT(Pool()->GetLiveCount() == 0, "사이클 종료 후 누수된 버퍼 존재");
    }
    std::cout << "  [OK] " << TestConfig::LIFECYCLE_CYCLES << " 사이클 누수/이중해제 없음" << std::endl;

    std::cout << "\n[PASS] 참조 카운팅 기본 테스트 완료!" << std::endl;
    g_testCount++;
}

//=============================================================================
// Phase 3-1: 브로드캐스트 시나리오 (멀티스레드)
//
// 실제 경로를 그대로 모사한다:
//   송신 스레드 : Alloc → 직렬화 → Seal → AddRef(N) → N개 워커 큐에 투입 → SubRef()
//   워커 스레드 : 큐에서 꺼내 PeekData 로 읽고(전송) → SubRef()
//
// 마지막 SubRef 가 어느 스레드에서 일어날지 정해져 있지 않다. 그 경합에서
// Free 가 정확히 라운드 수만큼만 일어나는지가 이 테스트의 핵심이다.
//=============================================================================
void Test_BroadcastMT()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 3-1] 브로드캐스트 시나리오 테스트 시작" << std::endl;
    std::cout << "  라운드: " << TestConfig::BROADCAST_ROUNDS << std::endl;
    std::cout << "========================================" << std::endl;

    const uint64_t PATTERN_MASK = 0x5A5A5A5A5A5A5A5AULL;
    const int threadCounts[] = { 2, 4, 8, 16 };

    struct WorkerQueue
    {
        std::mutex mutex;
        std::condition_variable cv;
        std::vector<UTBuffer*> pending;
        bool closed = false;
    };

    for (int threadCount : threadCounts)
    {
        std::cout << "\n  [타겟 " << threadCount << "개]" << std::endl;

        Pool()->ResetCounters();

        std::vector<std::unique_ptr<WorkerQueue>> queues;
        for (int t = 0; t < threadCount; ++t)
            queues.emplace_back(new WorkerQueue());

        std::atomic<uint64_t> sendOk(0);
        std::atomic<uint64_t> sendFail(0);
        std::vector<std::thread> workers;
        auto startTime = std::chrono::steady_clock::now();

        for (int t = 0; t < threadCount; ++t)
        {
            WorkerQueue* queue = queues[t].get();
            workers.emplace_back([queue, &sendOk, &sendFail, PATTERN_MASK]()
                {
                    std::vector<UTBuffer*> batch;

                    while (true)
                    {
                        {
                            std::unique_lock<std::mutex> lock(queue->mutex);
                            queue->cv.wait(lock, [queue]() { return queue->closed || !queue->pending.empty(); });

                            if (queue->pending.empty() && queue->closed)
                                break;

                            batch.swap(queue->pending);
                        }

                        for (UTBuffer* msg : batch)
                        {
                            // 봉인된 버퍼를 여러 워커가 동시에 읽는다 (PeekData 는 위치를 안 옮김)
                            char local[16];
                            uint64_t sequence = 0;
                            uint64_t mirrored = 0;

                            if (msg->PeekData(local, sizeof(local)) == (int)sizeof(local))
                            {
                                std::memcpy(&sequence, local, sizeof(sequence));
                                std::memcpy(&mirrored, local + sizeof(sequence), sizeof(mirrored));

                                if ((sequence ^ PATTERN_MASK) == mirrored)
                                    sendOk.fetch_add(1, std::memory_order_relaxed);
                                else
                                    sendFail.fetch_add(1, std::memory_order_relaxed);
                            }
                            else
                            {
                                sendFail.fetch_add(1, std::memory_order_relaxed);
                            }

                            msg->SubRef();   // 전송 완료 콜백
                        }

                        batch.clear();
                    }
                });
        }

        // --- 송신 스레드 (여기, 메인) ---
        for (uint64_t round = 0; round < TestConfig::BROADCAST_ROUNDS; ++round)
        {
            UTBuffer* msg = UTBuffer::Alloc();

            const uint64_t sequence = round;
            *msg << sequence << (uint64_t)(sequence ^ PATTERN_MASK);
            TEST_ASSERT(msg->GetDataSize() == 16, "브로드캐스트 페이로드 크기 불일치");

            msg->Seal();
            msg->AddRef(threadCount);   // 타겟 수만큼 한 번에 (원자연산 N→1)

            for (int t = 0; t < threadCount; ++t)
            {
                WorkerQueue* queue = queues[t].get();
                {
                    std::lock_guard<std::mutex> lock(queue->mutex);
                    queue->pending.push_back(msg);
                }
                queue->cv.notify_one();
            }

            msg->SubRef();   // 생성자 소유권 반납 — 이후 msg 접근 금지

            g_totalIterations++;
            PrintProgress("브로드캐스트", round + 1, TestConfig::BROADCAST_ROUNDS);
        }

        for (int t = 0; t < threadCount; ++t)
        {
            WorkerQueue* queue = queues[t].get();
            {
                std::lock_guard<std::mutex> lock(queue->mutex);
                queue->closed = true;
            }
            queue->cv.notify_all();
        }

        for (auto& worker : workers)
            worker.join();

        const uint64_t expectedSends = TestConfig::BROADCAST_ROUNDS * (uint64_t)threadCount;
        TEST_ASSERT(sendFail.load() == 0, "브로드캐스트 중 데이터 손상 발생");
        TEST_ASSERT(sendOk.load() == expectedSends, "전송 횟수 불일치 (누락)");
        TEST_ASSERT(Pool()->GetAllocCount() == (int64_t)TestConfig::BROADCAST_ROUNDS, "Alloc 총량 불일치");
        TEST_ASSERT(Pool()->GetFreeCount() == (int64_t)TestConfig::BROADCAST_ROUNDS,
            "Free 총량 불일치 — 회수 누락 또는 중복 회수");
        TEST_ASSERT(Pool()->GetLiveCount() == 0, "종료 후 누수된 버퍼 존재");

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count();
        std::cout << "    [OK] " << TestConfig::BROADCAST_ROUNDS << " 라운드 × " << threadCount
            << " 타겟 = " << expectedSends << " 전송, Free 정확히 "
            << Pool()->GetFreeCount() << "회 (소요: " << elapsed / 1000.0 << "초)" << std::endl;
    }

    std::cout << "\n[PASS] 브로드캐스트 시나리오 테스트 완료!" << std::endl;
    g_testCount++;
}

//=============================================================================
// Phase 3-2: 다중 스레드 Alloc/회수 스트레스
// 스레드마다 독립 버퍼를 대량으로 돌려 교차 오염 / 누수가 없는지 검증
//=============================================================================
void Test_ConcurrentLifecycle()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 3-2] 다중 스레드 수명 스트레스 시작" << std::endl;
    std::cout << "========================================" << std::endl;

    const int threadCounts[] = { 2, 4, 8, 16 };

    for (int threadCount : threadCounts)
    {
        std::cout << "\n  [스레드 " << threadCount << "개] 사이클/스레드: "
            << TestConfig::MT_CYCLES_PER_THREAD << std::endl;

        Pool()->ResetCounters();
        std::atomic<uint64_t> completed(0);
        std::vector<std::thread> threads;
        auto startTime = std::chrono::steady_clock::now();

        for (int t = 0; t < threadCount; ++t)
        {
            threads.emplace_back([t, &completed]()
                {
                    const uint64_t threadTag = 0xA5A5000000000000ULL | (uint64_t)t;

                    for (uint64_t cycle = 0; cycle < TestConfig::MT_CYCLES_PER_THREAD; ++cycle)
                    {
                        UTBuffer* msg = UTBuffer::Alloc();

                        const uint64_t sequence = threadTag ^ cycle;
                        *msg << sequence << (int)t << (double)cycle;

                        uint64_t outSequence = 0;
                        int outThreadId = -1;
                        double outCycle = -1.0;
                        *msg >> outSequence >> outThreadId >> outCycle;

                        TEST_ASSERT(outSequence == sequence, "스레드 간 데이터 오염 (시퀀스)");
                        TEST_ASSERT(outThreadId == t, "스레드 간 데이터 오염 (스레드 ID)");
                        TEST_ASSERT(outCycle == (double)cycle, "스레드 간 데이터 오염 (사이클)");
                        TEST_ASSERT(msg->GetDataSize() == 0, "소진 후 잔여 데이터 존재");

                        msg->SubRef();
                        completed++;
                    }
                });
        }

        for (auto& thread : threads)
            thread.join();

        const uint64_t expected = (uint64_t)threadCount * TestConfig::MT_CYCLES_PER_THREAD;
        TEST_ASSERT(completed.load() == expected, "완료 사이클 수 불일치");
        TEST_ASSERT(Pool()->GetAllocCount() == (int64_t)expected, "Alloc 총량 불일치");
        TEST_ASSERT(Pool()->GetFreeCount() == (int64_t)expected, "Free 총량 불일치");
        TEST_ASSERT(Pool()->GetLiveCount() == 0, "종료 후 누수된 버퍼 존재");

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count();
        std::cout << "    [OK] " << expected << " 사이클 완료, 누수 0 (소요: "
            << elapsed / 1000.0 << "초)" << std::endl;

        g_totalIterations += expected;
    }

    std::cout << "\n[PASS] 다중 스레드 수명 스트레스 완료!" << std::endl;
    g_testCount++;
}

//=============================================================================
// Phase 3-3: 전송 실패 경로 (A1)
//
// CIOCPServer::RequestSendMsg 의 소유권 계약:
//   "호출자는 RefCount >= 1 인 pMsg 를 넘긴다. 이 함수는 해당 1개 ref 를 소비한다."
//   세션 무효 / ABA 검출 / SendQ 상한 / Enqueue 실패 / 링버퍼 오버플로우 —
//   조기 반환 경로 전부가 SubRef() 를 정확히 1회 부른다.
//
// 성공 경로만 도는 테스트로는 이 계약이 깨져도 모른다. 실패를 섞어서
// "부여한 소유권 수 == 소비한 소유권 수" 가 유지되는지 본다.
//=============================================================================
namespace SendPath
{
    enum class Result
    {
        Sent,               // 정상 enqueue
        SessionInvalid,     // FindSession 실패
        AbaDetected,        // pin 후 sessionId 재확인 실패
        QueueOverflow       // SendQ 상한 / Enqueue 실패
    };

    // 실제 RequestSendMsg 의 소유권 동작만 떼어낸 모사본.
    // 어느 경로로 빠지든 SubRef 는 정확히 1회 — 이것이 검증 대상 계약이다.
    Result SimulateRequestSendMsg(UTBuffer* msg, uint32_t roll)
    {
        Result result;
        if (roll % 17 == 0)      result = Result::SessionInvalid;
        else if (roll % 23 == 0) result = Result::AbaDetected;
        else if (roll % 31 == 0) result = Result::QueueOverflow;
        else                     result = Result::Sent;

        // 전송 성공 경로에서는 버퍼를 실제로 읽는다 (봉인된 상태에서 PeekData)
        if (result == Result::Sent)
        {
            char local[16];
            msg->PeekData(local, sizeof(local));
        }

        msg->SubRef();
        return result;
    }
}

void Test_SendFailurePaths()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 3-3] 전송 실패 경로 테스트 시작" << std::endl;
    std::cout << "  라운드: " << TestConfig::SEND_FAILURE_ROUNDS << std::endl;
    std::cout << "========================================" << std::endl;

    const uint64_t PATTERN_MASK = 0x5A5A5A5A5A5A5A5AULL;

    // --- 0. 검증 장치가 실제로 누수를 잡는지 먼저 확인 (테스트의 테스트) ---
    //     소유권을 N개 부여하고 N-1 번만 소비하면 회수가 안 되어야 한다.
    {
        Pool()->ResetCounters();
        UTBuffer* msg = UTBuffer::Alloc();
        *msg << (uint64_t)1 << (uint64_t)(1 ^ PATTERN_MASK);
        msg->Seal();

        const int64_t targets = 10;
        msg->AddRef(targets);
        for (int64_t i = 0; i < targets - 1; ++i)   // 일부러 1회 덜 소비
            msg->SubRef();
        msg->SubRef();                              // 빌더 몫

        TEST_ASSERT(Pool()->GetFreeCount() == 0, "소유권이 남았는데 회수됨");
        TEST_ASSERT(Pool()->GetLiveCount() == 1, "누수가 감지되지 않음 — 검증 장치가 무력함");

        msg->SubRef();                              // 정리
        TEST_ASSERT(Pool()->GetLiveCount() == 0, "정리 실패");
    }
    std::cout << "  [OK] 검증 장치 자체 확인 — 소비 1회 누락을 실제로 잡아냄" << std::endl;

    // --- 1. 실패를 섞은 브로드캐스트 (단일 스레드) ---
    {
        Pool()->ResetCounters();
        std::mt19937 gen(0xFA11U);
        uint64_t sentCount = 0, failCount = 0;

        for (uint64_t round = 0; round < TestConfig::SEND_FAILURE_ROUNDS; ++round)
        {
            UTBuffer* msg = UTBuffer::Alloc();
            *msg << round << (uint64_t)(round ^ PATTERN_MASK);
            msg->Seal();

            // 실제 코드처럼 유효 타겟을 선카운트한 뒤 배치 AddRef
            const int64_t validCount = (int64_t)(gen() % 17);   // 0~16명
            if (validCount > 0)
                msg->AddRef(validCount);

            for (int64_t t = 0; t < validCount; ++t)
            {
                if (SendPath::SimulateRequestSendMsg(msg, gen()) == SendPath::Result::Sent)
                    ++sentCount;
                else
                    ++failCount;
            }

            msg->SubRef();   // 빌더가 넘긴 소유권 1 회수 (타겟 0명이어도 안전)

            g_totalIterations++;
            PrintProgress("전송 실패 혼합", round + 1, TestConfig::SEND_FAILURE_ROUNDS);
        }

        TEST_ASSERT(failCount > 0, "실패 경로가 한 번도 발생하지 않음 — 테스트가 무의미");
        TEST_ASSERT(Pool()->GetAllocCount() == (int64_t)TestConfig::SEND_FAILURE_ROUNDS, "Alloc 총량 불일치");
        TEST_ASSERT(Pool()->GetFreeCount() == (int64_t)TestConfig::SEND_FAILURE_ROUNDS,
            "Free 총량 불일치 — 실패 경로에서 소유권이 새거나 중복 회수됨");
        TEST_ASSERT(Pool()->GetLiveCount() == 0, "종료 후 누수된 버퍼 존재");

        std::cout << "  [OK] 실패 혼합 " << TestConfig::SEND_FAILURE_ROUNDS << " 라운드 "
            << "(성공 " << sentCount << " / 실패 " << failCount << ") — 누수 0" << std::endl;
    }

    // --- 2. 전 타겟 실패 (세션이 전부 끊긴 상황) ---
    {
        Pool()->ResetCounters();

        for (uint64_t round = 0; round < TestConfig::SEND_FAILURE_ROUNDS / 10; ++round)
        {
            UTBuffer* msg = UTBuffer::Alloc();
            *msg << round << (uint64_t)(round ^ PATTERN_MASK);
            msg->Seal();

            const int64_t validCount = 8;
            msg->AddRef(validCount);
            for (int64_t t = 0; t < validCount; ++t)
                SendPath::SimulateRequestSendMsg(msg, 17);   // 항상 SessionInvalid

            msg->SubRef();
        }

        const int64_t rounds = (int64_t)(TestConfig::SEND_FAILURE_ROUNDS / 10);
        TEST_ASSERT(Pool()->GetFreeCount() == rounds, "전 타겟 실패 시 Free 총량 불일치");
        TEST_ASSERT(Pool()->GetLiveCount() == 0, "전 타겟 실패 시 누수 발생");
        std::cout << "  [OK] 전 타겟 실패 " << rounds << " 라운드 — 누수 0" << std::endl;
    }

    // --- 3. 멀티스레드: 워커가 실패/성공을 섞어 소비 ---
    {
        const int threadCounts[] = { 4, 16 };

        for (int threadCount : threadCounts)
        {
            Pool()->ResetCounters();

            std::mutex mutex;
            std::condition_variable cv;
            std::vector<UTBuffer*> pending;
            bool closed = false;
            std::atomic<uint64_t> consumed(0);
            std::atomic<uint64_t> corrupted(0);

            std::vector<std::thread> workers;
            for (int t = 0; t < threadCount; ++t)
            {
                workers.emplace_back([&]()
                    {
                        std::mt19937 gen(0xBEEF0000U + (uint32_t)std::hash<std::thread::id>()(std::this_thread::get_id()));
                        std::vector<UTBuffer*> batch;

                        while (true)
                        {
                            {
                                std::unique_lock<std::mutex> lock(mutex);
                                cv.wait(lock, [&]() { return closed || !pending.empty(); });
                                if (pending.empty() && closed)
                                    break;
                                batch.swap(pending);
                            }

                            for (UTBuffer* msg : batch)
                            {
                                // 봉인 버퍼 읽기 검증 후, 성공/실패 무관하게 ref 1개 소비
                                char local[16];
                                uint64_t sequence = 0, mirrored = 0;
                                if (msg->PeekData(local, sizeof(local)) == (int)sizeof(local))
                                {
                                    std::memcpy(&sequence, local, sizeof(sequence));
                                    std::memcpy(&mirrored, local + sizeof(sequence), sizeof(mirrored));
                                    if ((sequence ^ PATTERN_MASK) != mirrored)
                                        corrupted.fetch_add(1, std::memory_order_relaxed);
                                }
                                else
                                {
                                    corrupted.fetch_add(1, std::memory_order_relaxed);
                                }

                                SendPath::SimulateRequestSendMsg(msg, gen());
                                consumed.fetch_add(1, std::memory_order_relaxed);
                            }
                            batch.clear();
                        }
                    });
            }

            const uint64_t rounds = TestConfig::SEND_FAILURE_ROUNDS / 4;
            for (uint64_t round = 0; round < rounds; ++round)
            {
                UTBuffer* msg = UTBuffer::Alloc();
                *msg << round << (uint64_t)(round ^ PATTERN_MASK);
                msg->Seal();
                msg->AddRef(threadCount);

                {
                    std::lock_guard<std::mutex> lock(mutex);
                    for (int t = 0; t < threadCount; ++t)
                        pending.push_back(msg);
                }
                cv.notify_all();

                msg->SubRef();
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                closed = true;
            }
            cv.notify_all();
            for (auto& worker : workers)
                worker.join();

            TEST_ASSERT(corrupted.load() == 0, "실패 혼합 중 데이터 손상 발생");
            TEST_ASSERT(consumed.load() == rounds * (uint64_t)threadCount, "소비 횟수 불일치");
            TEST_ASSERT(Pool()->GetFreeCount() == (int64_t)rounds, "MT 실패 혼합 Free 총량 불일치");
            TEST_ASSERT(Pool()->GetLiveCount() == 0, "MT 실패 혼합 후 누수 발생");

            std::cout << "  [OK] MT 실패 혼합 (워커 " << threadCount << ") "
                << rounds << " 라운드 — 누수 0" << std::endl;
            g_totalIterations += rounds;
        }
    }

    std::cout << "\n[PASS] 전송 실패 경로 테스트 완료!" << std::endl;
    g_testCount++;
}

//=============================================================================
// Phase 3-4: 타겟 0명 브로드캐스트 (A2)
//
// 실제 코드는 유효 타겟이 0명이면 배치 AddRef 를 아예 하지 않고,
// 빌더가 넘긴 소유권 1개만 SubRef 로 회수한다.
//   if (validCount > 0) pMsg->AddRef(validCount);
//   ...
//   pMsg->SubRef();   // 타겟 0명이어도 안전 회수
// AddRef(0) 을 부르는 변형도 같은 결과여야 한다.
//=============================================================================
void Test_ZeroTargetBroadcast()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 3-4] 타겟 0명 브로드캐스트 테스트 시작" << std::endl;
    std::cout << "  라운드: " << TestConfig::ZERO_TARGET_ROUNDS << std::endl;
    std::cout << "========================================" << std::endl;

    // --- 1. AddRef 자체를 생략하는 경로 (실제 코드) ---
    {
        Pool()->ResetCounters();

        for (uint64_t round = 0; round < TestConfig::ZERO_TARGET_ROUNDS; ++round)
        {
            UTBuffer* msg = UTBuffer::Alloc();
            *msg << round;
            msg->Seal();

            const int64_t validCount = 0;
            if (validCount > 0)
                msg->AddRef(validCount);

            TEST_ASSERT(msg->_RefCount.load() == 1, "타겟 0명인데 RefCount 가 1이 아님");

            msg->SubRef();

            g_totalIterations++;
            PrintProgress("타겟 0명", round + 1, TestConfig::ZERO_TARGET_ROUNDS);
        }

        TEST_ASSERT(Pool()->GetFreeCount() == (int64_t)TestConfig::ZERO_TARGET_ROUNDS,
            "타겟 0명 경로 Free 총량 불일치");
        TEST_ASSERT(Pool()->GetLiveCount() == 0, "타겟 0명 경로에서 누수 발생");
    }
    std::cout << "  [OK] AddRef 생략 경로 — Free 정확히 " << TestConfig::ZERO_TARGET_ROUNDS << "회" << std::endl;

    // --- 2. AddRef(0) 을 실제로 호출하는 변형 ---
    {
        Pool()->ResetCounters();
        const uint64_t rounds = TestConfig::ZERO_TARGET_ROUNDS / 10;

        for (uint64_t round = 0; round < rounds; ++round)
        {
            UTBuffer* msg = UTBuffer::Alloc();
            *msg << round;
            msg->Seal();

            msg->AddRef(0);   // 0 을 그대로 넘겨도 카운트가 흔들리면 안 된다
            TEST_ASSERT(msg->_RefCount.load() == 1, "AddRef(0) 이 RefCount 를 변경함");

            msg->SubRef();
        }

        TEST_ASSERT(Pool()->GetFreeCount() == (int64_t)rounds, "AddRef(0) 경로 Free 총량 불일치");
        TEST_ASSERT(Pool()->GetLiveCount() == 0, "AddRef(0) 경로에서 누수 발생");
    }
    std::cout << "  [OK] AddRef(0) 명시 호출 — RefCount 불변, 회수 정상" << std::endl;

    // --- 3. 타겟 수가 0과 N 사이를 오가는 혼합 ---
    {
        Pool()->ResetCounters();
        std::mt19937 gen(0x2E20U);
        const uint64_t rounds = TestConfig::ZERO_TARGET_ROUNDS / 5;
        uint64_t zeroRounds = 0;

        for (uint64_t round = 0; round < rounds; ++round)
        {
            UTBuffer* msg = UTBuffer::Alloc();
            *msg << round;
            msg->Seal();

            const int64_t validCount = (int64_t)(gen() % 4);   // 0~3, 0이 자주 나온다
            if (validCount == 0)
                ++zeroRounds;

            if (validCount > 0)
                msg->AddRef(validCount);
            for (int64_t t = 0; t < validCount; ++t)
                msg->SubRef();

            msg->SubRef();
        }

        TEST_ASSERT(zeroRounds > 0, "0명 라운드가 한 번도 발생하지 않음");
        TEST_ASSERT(Pool()->GetFreeCount() == (int64_t)rounds, "혼합 경로 Free 총량 불일치");
        TEST_ASSERT(Pool()->GetLiveCount() == 0, "혼합 경로에서 누수 발생");
        std::cout << "  [OK] 0명/N명 혼합 " << rounds << " 라운드 (0명 " << zeroRounds << "회) — 누수 0" << std::endl;
    }

    std::cout << "\n[PASS] 타겟 0명 브로드캐스트 테스트 완료!" << std::endl;
    g_testCount++;
}

//=============================================================================
// Phase 3-5: 단건 + 배치 AddRef 혼합 (A3)
//
// 실제 코드는 한 버퍼에 두 방식을 섞어 쓴다:
//   pMsg->AddRef();                  // 점프 폴백 / 직송 등록 1건당 소유권 1
//   pMsg->AddRef(validCount);        // 섹터 팬아웃 타겟 수만큼 배치
//   pMsg->SubRef();                  // 빌더 몫 회수
// 부여 총합과 소비 총합이 맞으면 Free 는 정확히 1회여야 한다.
//=============================================================================
void Test_MixedAddRef()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 3-5] 단건+배치 AddRef 혼합 테스트 시작" << std::endl;
    std::cout << "  라운드: " << TestConfig::MIXED_ADDREF_ROUNDS << std::endl;
    std::cout << "========================================" << std::endl;

    // --- 1. 단일 스레드 혼합 ---
    {
        Pool()->ResetCounters();
        std::mt19937 gen(0x3A3AU);
        uint64_t totalGrants = 0;

        for (uint64_t round = 0; round < TestConfig::MIXED_ADDREF_ROUNDS; ++round)
        {
            UTBuffer* msg = UTBuffer::Alloc();
            *msg << round;
            msg->Seal();

            int64_t grants = 0;

            // 직송 등록 — 1건당 단건 AddRef
            const int64_t directSends = (int64_t)(gen() % 5);
            for (int64_t d = 0; d < directSends; ++d)
            {
                msg->AddRef();
                ++grants;
            }

            // 섹터 팬아웃 — 타겟 수만큼 배치 AddRef
            const int64_t fanoutTargets = (int64_t)(gen() % 9);
            if (fanoutTargets > 0)
            {
                msg->AddRef(fanoutTargets);
                grants += fanoutTargets;
            }

            // 점프 폴백 — 단건 AddRef 하나 더 (확률적)
            const bool jumpFallback = (gen() % 3) == 0;
            if (jumpFallback)
            {
                msg->AddRef();
                ++grants;
            }

            TEST_ASSERT(msg->_RefCount.load() == grants + 1, "혼합 AddRef 후 RefCount 불일치");

            // 소비 — 부여한 순서와 무관하게 총량만 맞추면 된다
            for (int64_t g = 0; g < grants; ++g)
            {
                TEST_ASSERT(Pool()->GetLiveCount() == 1, "소유권이 남았는데 회수됨");
                msg->SubRef();
            }

            msg->SubRef();   // 빌더 몫
            totalGrants += (uint64_t)grants;

            g_totalIterations++;
            PrintProgress("AddRef 혼합", round + 1, TestConfig::MIXED_ADDREF_ROUNDS);
        }

        TEST_ASSERT(Pool()->GetFreeCount() == (int64_t)TestConfig::MIXED_ADDREF_ROUNDS,
            "혼합 AddRef Free 총량 불일치");
        TEST_ASSERT(Pool()->GetLiveCount() == 0, "혼합 AddRef 후 누수 발생");
        std::cout << "  [OK] 단일 스레드 혼합 " << TestConfig::MIXED_ADDREF_ROUNDS
            << " 라운드 (총 부여 " << totalGrants << ") — 누수 0" << std::endl;
    }

    // --- 2. 멀티스레드: 부여와 소비가 서로 다른 스레드에서 일어난다 ---
    {
        const int CONSUMER_COUNT = 8;
        Pool()->ResetCounters();

        std::mutex mutex;
        std::condition_variable cv;
        std::vector<UTBuffer*> pending;
        bool closed = false;
        std::atomic<uint64_t> consumed(0);

        std::vector<std::thread> consumers;
        for (int t = 0; t < CONSUMER_COUNT; ++t)
        {
            consumers.emplace_back([&]()
                {
                    std::vector<UTBuffer*> batch;
                    while (true)
                    {
                        {
                            std::unique_lock<std::mutex> lock(mutex);
                            cv.wait(lock, [&]() { return closed || !pending.empty(); });
                            if (pending.empty() && closed)
                                break;
                            batch.swap(pending);
                        }

                        for (UTBuffer* msg : batch)
                        {
                            msg->SubRef();
                            consumed.fetch_add(1, std::memory_order_relaxed);
                        }
                        batch.clear();
                    }
                });
        }

        std::mt19937 gen(0x9C9CU);
        const uint64_t rounds = TestConfig::MIXED_ADDREF_ROUNDS / 4;
        uint64_t expectedConsumes = 0;

        for (uint64_t round = 0; round < rounds; ++round)
        {
            UTBuffer* msg = UTBuffer::Alloc();
            *msg << round;
            msg->Seal();

            const int64_t singles = (int64_t)(gen() % 3);       // 단건
            const int64_t batchCount = (int64_t)(gen() % 6);    // 배치

            for (int64_t d = 0; d < singles; ++d)
                msg->AddRef();
            if (batchCount > 0)
                msg->AddRef(batchCount);

            const int64_t grants = singles + batchCount;
            expectedConsumes += (uint64_t)grants;

            if (grants > 0)
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (int64_t g = 0; g < grants; ++g)
                    pending.push_back(msg);
            }
            cv.notify_all();

            msg->SubRef();   // 빌더 몫 — 소비자보다 먼저 반납될 수 있다
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            closed = true;
        }
        cv.notify_all();
        for (auto& consumer : consumers)
            consumer.join();

        TEST_ASSERT(consumed.load() == expectedConsumes, "MT 혼합 소비 횟수 불일치");
        TEST_ASSERT(Pool()->GetFreeCount() == (int64_t)rounds, "MT 혼합 Free 총량 불일치");
        TEST_ASSERT(Pool()->GetLiveCount() == 0, "MT 혼합 후 누수 발생");
        std::cout << "  [OK] MT 혼합 (소비자 " << CONSUMER_COUNT << ") " << rounds
            << " 라운드, 소비 " << consumed.load() << "회 — 누수 0" << std::endl;
        g_totalIterations += rounds;
    }

    std::cout << "\n[PASS] 단건+배치 AddRef 혼합 테스트 완료!" << std::endl;
    g_testCount++;
}

//=============================================================================
// Phase 4: 잠재 결함 점검 (비파괴)
//
// 아래 두 가지는 코드를 읽어 찾은 의심 지점이다. 실제로 트리거하면 버퍼 밖으로
// 쓰거나 쓰레기 값을 읽게 되므로, 여기서는 "위험 조건이 성립하는지"만 계산으로
// 확인하고 경고만 남긴다. 죽이지 않는다.
//=============================================================================
void Test_KnownHazards()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 4] 잠재 결함 점검 (비파괴)" << std::endl;
    std::cout << "========================================" << std::endl;

    // --- 1. Clear 없이 읽고 다시 쓰는 경우 ---
    //
    // IsFull(size) 은 _DataSize 기준이다:  _DataSize + size > _BufferSize - HEADER_SIZE
    // 그런데 실제 쓰기는 _rear 위치에 한다. 읽기가 진행되면 _DataSize 는 줄지만
    // _rear 는 그대로이므로, "여유 있음" 판정과 실제 남은 공간이 어긋난다.
    {
        const int BUFFER_SIZE = 64;
        const int USABLE = BUFFER_SIZE - HEADER_SIZE;

        UTBuffer buffer(BUFFER_SIZE);

        char filler[USABLE];
        std::memset(filler, 0x5A, sizeof(filler));
        TEST_ASSERT(buffer.SetData(filler, USABLE) == USABLE, "채우기 실패");

        char drain[USABLE];
        TEST_ASSERT(buffer.GetData(drain, USABLE) == USABLE, "비우기 실패");
        TEST_ASSERT(buffer.GetDataSize() == 0, "비운 뒤 DataSize 가 0이 아님");

        // 이 시점: _DataSize=0 (여유 있어 보임), _rear=USABLE (실제로는 꽉 참)
        const char* bufferEnd = buffer.GetHeaderBufferPtr() + buffer.GetBufferSize();
        const bool saysHasRoom = (buffer.IsFull(1) == false);
        const bool writePtrPastEnd = (buffer.GetWriteBufferPtr() >= bufferEnd);

        std::cout << "  IsFull(1) == false (여유 있다고 판정) : " << (saysHasRoom ? "예" : "아니오") << std::endl;
        std::cout << "  쓰기 포인터가 버퍼 끝 이상            : " << (writePtrPastEnd ? "예" : "아니오") << std::endl;

        TEST_WARN(!(saysHasRoom && writePtrPastEnd),
            "Clear 없이 전부 읽은 뒤 다시 쓰면 버퍼 밖에 기록된다 "
            "(IsFull 은 _DataSize 기준, 쓰기는 _rear 위치). "
            "→ 사용 계약: 재사용 전 반드시 Clear() 또는 Alloc()");
    }

    // --- 2. 부분 읽기 상태의 버퍼를 복사 대입하는 경우 ---
    //
    // operator= 는 _Buff 선두에서 HEADER_SIZE + _DataSize 만큼만 복사하는데,
    // 유효 데이터 구간은 [HEADER_SIZE + _front, HEADER_SIZE + _rear) 다.
    // _front > 0 이면 복사 길이가 모자라 뒷부분이 빠진다.
    {
        UTBuffer src(64);
        UTBuffer dst(64);

        for (int i = 0; i < 5; ++i)
            src << (int)(0x11111111 * (i + 1));

        int consumed = 0;
        src >> consumed;                       // 앞의 4바이트를 읽어 _front 를 밀어둔다
        TEST_ASSERT(consumed == 0x11111111, "선행 읽기 값 불일치");

        dst = src;                             // 부분 읽기 상태에서 복사

        TEST_ASSERT(dst.GetDataSize() == src.GetDataSize(), "복사본 DataSize 불일치");

        bool tailMatches = true;
        for (int i = 1; i < 5; ++i)
        {
            int value = 0;
            dst >> value;
            if (value != (int)(0x11111111 * (i + 1)))
                tailMatches = false;
        }

        std::cout << "  부분 읽기 상태 복사본의 뒷부분 일치    : " << (tailMatches ? "예" : "아니오") << std::endl;

        TEST_WARN(tailMatches,
            "_front > 0 인 버퍼를 operator= 로 복사하면 뒷부분이 복사되지 않는다 "
            "(복사 길이 HEADER_SIZE + _DataSize < 유효 구간 끝 HEADER_SIZE + _rear). "
            "→ 사용 계약: 읽기 전(_front==0) 상태에서만 복사 대입");
    }

    // --- 3. 소유권을 초과 반납한 경우 (A4) ---
    //
    // SubRef 는 fetch_sub 결과가 정확히 1일 때만 Free 를 부른다.
    // AddRef 보다 SubRef 를 많이 부르면 카운트가 음수로 내려가고 회수는 영영 안 된다.
    // 실제로 SubRef 를 한 번 더 부르면 이미 회수된 객체를 만지게 되므로,
    // 여기서는 RefCount 만 0 으로 맞춰놓고 SubRef 의 분기만 관찰한다.
    {
        Pool()->ResetCounters();
        UTBuffer* msg = UTBuffer::Alloc();

        msg->_RefCount.store(0);
        msg->SubRef();

        const bool freed = (Pool()->GetFreeCount() == 1);
        std::cout << "  초과 반납 후 RefCount                  : " << msg->_RefCount.load() << std::endl;
        std::cout << "  회수(Free) 되었는가                    : " << (freed ? "예" : "아니오") << std::endl;

#ifdef USE_FIXED_SERIALBUFFER
        // 수정본은 되돌리지는 못해도 검출은 한다
        TEST_ASSERT(msg->HasRefUnderflow(), "수정본이 초과 반납을 검출하지 못함");
        std::cout << "  (수정본) 초과 반납 검출 플래그         : 섰음 — 조용하지 않음" << std::endl;
#endif

        TEST_WARN(freed,
            "소유권을 초과 반납하면 Free 가 호출되지 않아 버퍼가 누수된다 "
            "(SubRef 는 fetch_sub 결과가 정확히 1일 때만 회수). "
            "→ 사용 계약: AddRef 횟수와 SubRef 횟수를 정확히 맞출 것");

        msg->_RefCount.store(1);   // 정리
        msg->SubRef();
        TEST_ASSERT(Pool()->GetLiveCount() == 0, "A4 점검 후 정리 실패");
    }

    // --- 4. Release() 이중 호출 (B1) ---
    //
    //   void Release(void) { delete[] _Buff; }   // _Buff 를 null 로 만들지 않는다
    //   ~UTBuffer()   { Release(); }        // 소멸자도 같은 함수를 부른다
    //
    // Release() 가 public 이라 외부에서 부를 수 있고, 그 뒤 소멸하면 두 번째 delete[] 가 일어난다.
    // 실제로 이중 해제를 일으키면 프로세스가 죽으므로 "Release() 후에도 포인터가 그대로 남는지"만
    // 확인하고(역참조하지 않는다), 곧바로 Initialize() 로 새 버퍼를 물려 소멸자를 안전하게 만든다.
    {
        UTBuffer buffer(64);
        char* before = buffer.GetHeaderBufferPtr();
        TEST_ASSERT(before != nullptr, "초기 버퍼가 nullptr");

        buffer.Release();

        char* after = buffer.GetHeaderBufferPtr();   // 값만 비교, 역참조 금지
        const bool danglingKept = (after == before);

        std::cout << "  Release() 후 버퍼 포인터가 그대로인가   : " << (danglingKept ? "예" : "아니오") << std::endl;

        TEST_WARN(!danglingKept,
            "버퍼 수명 관리가 생성자/소멸자 밖으로 새어 있다. "
            "(1) Release() 가 _Buff 를 null 로 만들지 않는데 public 이고 소멸자도 Release() 를 "
            "부르므로, 명시 호출 후 소멸 시 이중 해제가 된다. "
            "(2) Initialize() 도 이전 _Buff 를 해제하지 않고 덮어쓰므로 두 번 부르면 첫 버퍼가 샌다. "
            "→ 둘 다 현재 외부 호출처 없음(잠재). "
            "계약: Release()/Initialize() 를 직접 부르지 말 것 (생성자·소멸자에 맡길 것)");

        buffer.Initialize(64);   // 소멸자가 유효한 버퍼를 해제하도록 복구
    }

    // --- 5. Initialize() 재호출 (B2) — 실행 점검하지 않는 이유 ---
    //
    //   void Initialize(int BufferSize) { _Buff = new char[BufferSize]; ... }
    //
    // 이전 _Buff 를 해제하지 않고 덮어쓰므로 두 번 부르면 첫 버퍼가 샌다.
    // 그런데 이 사실은 실행으로 안전하게 확인할 수 없다:
    //   · "이전 버퍼가 해제됐는지"를 UB 없이 관찰할 방법이 없고,
    //   · 테스트가 대신 delete[] 해 주면, 나중에 원본이 고쳐졌을 때 그 해제가
    //     이중 해제가 되어 테스트가 수정을 방해한다.
    // 초기 버전이 실제로 그렇게 짜여 있었고 역방향 뮤테이션(고치면 경고가
    // 사라지는지 확인)에서 걸렸다. 그래서 위 B1 경고문에 사실로 병기만 한다.

    // --- 6. 문자열 길이 오버플로우 (B3) ---
    //
    //   short Len = (short)strlen(Value);
    //   if (IsFull(sizeof(Len) + Len)) return *this;
    //
    // 32767 을 넘는 문자열이면 Len 이 음수가 되고, 그 음수가 그대로 가드 계산에 들어간다.
    // 실제로 쓰기를 시키면 SetData 가 음수 크기로 _rear 를 뒤로 밀어 버퍼 앞쪽 밖을 건드리므로,
    // 여기서는 가드가 막아주는지만 계산으로 확인한다 (쓰기는 실행하지 않는다).
    {
        const size_t hugeLength = 40000;              // 32767 초과
        const short truncated = (short)hugeLength;    // 음수로 잘린다

        UTBuffer big((int)hugeLength + 16);
        const bool guardBlocks = big.IsFull((int)(sizeof(short) + truncated));

        std::cout << "  40000자 문자열의 기록 길이              : " << truncated << std::endl;
        std::cout << "  IsFull 가드가 막아주는가                : " << (guardBlocks ? "예" : "아니오") << std::endl;

        TEST_WARN(truncated > 0 || guardBlocks,
            "32767 을 넘는 문자열은 길이가 음수로 잘리고 그 음수가 IsFull 계산에 그대로 들어가 "
            "가드를 통과한다. 뒤이은 SetData 가 음수 크기로 _rear 를 뒤로 민다. "
            "→ 기본 버퍼(1460)에서는 도달 불가. 32KB 이상 버퍼를 쓸 때만 위험");
    }

    // --- 7. operator= 크기 초과 시 조용한 실패 (B4) ---
    //
    //   int srcTotalSize = HEADER_SIZE + SrcMsg._DataSize;
    //   if (srcTotalSize > _BufferSize) return *this;   // 아무 표시 없이 무시
    {
        UTBuffer src(1000);
        for (int i = 0; i < 100; ++i)
            src << (int)i;                 // 400 바이트

        UTBuffer dst(64);             // 담을 수 없는 크기
        const int beforeSize = dst.GetDataSize();
        dst = src;
        const int afterSize = dst.GetDataSize();

        const bool silentlyIgnored = (afterSize == beforeSize);
        std::cout << "  담을 수 없는 복사가 무시되었는가        : " << (silentlyIgnored ? "예" : "아니오") << std::endl;

        // 오염은 없어야 한다 — 이건 통과해야 할 조건
        TEST_ASSERT(silentlyIgnored, "크기 초과 복사가 대상 버퍼를 오염시킴");

#ifdef USE_FIXED_SERIALBUFFER
        TEST_ASSERT(dst.HasCopyFailed(), "수정본이 복사 실패를 표시하지 못함");
        std::cout << "  (수정본) 복사 실패 플래그               : 섰음 — 호출부가 확인 가능" << std::endl;
#endif

        TEST_WARN(!silentlyIgnored,
            "operator= 는 대상 버퍼가 작으면 복사를 건너뛰지만 반환값이 없어 호출부가 실패를 알 수 없다. "
            "복사됐다고 믿고 쓰면 빈 버퍼를 전송하게 된다. "
            "→ 사용 계약: 복사 대입 전 대상 버퍼 크기를 직접 확인할 것");
    }

    // --- 8. 음수 크기가 위치 이동 함수를 통과한다 (C2 에서 발견) ---
    //
    //   MoveWritePos: IsFull(-1) 이 false 라 통과 → _rear -= 1, _DataSize -= 1
    //   MoveReadPos : (_front + -1 > _rear) 가 false 라 else 분기 → _DataSize -= (-1)
    //
    // 둘 다 "성공"으로 보이는 값을 돌려주면서 내부 상태를 망가뜨린다.
    // 버퍼를 버리기만 하면 안전하므로 여기서 실제로 확인한다.
    {
        UTBuffer writeBuf(128);
        writeBuf.MoveWritePos(-1);
        const int afterWrite = writeBuf.GetDataSize();

        UTBuffer readBuf(128);
        readBuf.MoveReadPos(-1);
        const int afterRead = readBuf.GetDataSize();

        std::cout << "  MoveWritePos(-1) 후 DataSize            : " << afterWrite << std::endl;
        std::cout << "  MoveReadPos(-1) 후 DataSize             : " << afterRead << std::endl;

        TEST_WARN(afterWrite == 0 && afterRead == 0,
            "음수 크기가 MoveWritePos/MoveReadPos 를 통과해 내부 상태를 망가뜨린다. "
            "MoveWritePos(-1) 은 쓰기 위치를 페이로드 앞쪽으로 밀고, "
            "MoveReadPos(-1) 은 없던 데이터가 있는 것처럼 DataSize 를 늘린다. "
            "→ IsFull/IsEmpty 에 음수 가드를 넣으면 함께 막힌다");
    }

    // --- 9. SetData/GetData 의 음수·nullptr 은 실행 점검하지 않는다 ---
    //
    // 둘 다 memcpy_s 까지 내려간다. MSVC 의 memcpy_s 는 잘못된 인자에서
    // invalid parameter handler 를 부르고 기본 설정이면 프로세스를 죽인다.
    // 즉 이 경로를 테스트로 실행하면 테스트가 서버를 죽이는 형태가 된다.
    // (음수 size 가 가드를 통과한다는 사실 자체는 위 6번·8번에서 확인됨)
    std::cout << "  SetData/GetData 의 음수·nullptr          : 실행 점검 제외 (memcpy_s 가 프로세스를 죽임)" << std::endl;

    if (g_warnCount.load() == 0)
        std::cout << "\n[PASS] 잠재 결함 점검 — 경고 없음" << std::endl;
    else
        std::cout << "\n[WARN] 잠재 결함 점검 — 경고 " << g_warnCount.load() << "건 (위 내용 확인)" << std::endl;

    g_testCount++;
}

//=============================================================================
// Phase 5: 원본 ↔ 수정본 동등성 (와이어 호환)
//
// 14종 테스트는 UTBuffer 가 가리키는 한쪽만 돌린다. 둘 다 PASS 라는 것과
// "둘이 같은 바이트를 만든다"는 것은 다른 얘기다. 여기서는 두 구현을 직접
// 맞대어 비교한다.
//
// 이게 필요한 이유: 수정본을 서버에 반영할 때 송신·수신 양쪽을 동시에
// 바꾸지 않아도 되려면, 수정본이 만든 패킷을 원본이 그대로 파싱할 수 있어야
// 한다. 그 호환성을 여기서 증명한다.
//
// 프리리스트를 쓰지 않고 스택 객체로 비교한다 (두 클래스가 각자의 정적
// 프리리스트를 갖고 있어, 한쪽만 초기화된 상태에서도 안전하게 돌리기 위함).
//=============================================================================
void Test_Equivalence()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 5] 원본 ↔ 수정본 동등성 테스트 시작" << std::endl;
    std::cout << "  라운드: " << TestConfig::EQUIVALENCE_ROUNDS << std::endl;
    std::cout << "========================================" << std::endl;

    std::mt19937_64 gen(0xE9U);
    std::uniform_int_distribution<int> fieldCountDis(1, 60);
    std::uniform_int_distribution<int> typeDis(0, 9);

    struct Field { int type; uint64_t raw; };
    std::vector<Field> fields;
    fields.reserve(60);

    uint64_t comparedBytes = 0;

    for (uint64_t round = 1; round <= TestConfig::EQUIVALENCE_ROUNDS; ++round)
    {
        ::CSerialBuffer   origin(MSG_DEFAULT_SIZE);
        Fixed::CSerialBuffer fixed(Fixed::MSG_DEFAULT_SIZE);

        fields.clear();
        const int fieldCount = fieldCountDis(gen);

        // --- 같은 값 시퀀스를 양쪽에 똑같이 직렬화 ---
        for (int f = 0; f < fieldCount; ++f)
        {
            Field field;
            field.type = typeDis(gen);
            field.raw = gen();
            fields.push_back(field);

            switch (field.type)
            {
            case 0: origin << (uint8_t)field.raw;  fixed << (uint8_t)field.raw;  break;
            case 1: origin << (char)field.raw;     fixed << (char)field.raw;     break;
            case 2: origin << (short)field.raw;    fixed << (short)field.raw;    break;
            case 3: origin << (uint16_t)field.raw; fixed << (uint16_t)field.raw; break;
            case 4: origin << (int)field.raw;      fixed << (int)field.raw;      break;
            case 5: origin << (uint32_t)field.raw; fixed << (uint32_t)field.raw; break;
            case 6: origin << (int64_t)field.raw;  fixed << (int64_t)field.raw;  break;
            case 7: origin << (uint64_t)field.raw; fixed << (uint64_t)field.raw; break;
            case 8:
            {
                const float value = (float)(field.raw % 100000) / 7.0f;
                origin << value; fixed << value;
                break;
            }
            default:
            {
                const double value = (double)(field.raw % 1000000) / 13.0;
                origin << value; fixed << value;
                break;
            }
            }
        }

        // --- 1. 크기와 페이로드 바이트가 동일해야 한다 ---
        const int originSize = origin.GetDataSize();
        const int fixedSize = fixed.GetDataSize();
        TEST_ASSERT(originSize == fixedSize, "직렬화 크기가 두 구현에서 다름");
        TEST_ASSERT(std::memcmp(origin.GetPayloadBufferPtr(), fixed.GetPayloadBufferPtr(),
            (size_t)originSize) == 0, "직렬화 바이트가 두 구현에서 다름");
        comparedBytes += (uint64_t)originSize;

        // --- 2. 헤더 영역까지 포함한 전송 바이트도 동일해야 한다 ---
        const short payloadLen = (short)originSize;
        std::memcpy(origin.GetHeaderBufferPtr(), &payloadLen, HEADER_SIZE);
        std::memcpy(fixed.GetHeaderBufferPtr(), &payloadLen, Fixed::HEADER_SIZE);
        TEST_ASSERT(std::memcmp(origin.GetHeaderBufferPtr(), fixed.GetHeaderBufferPtr(),
            (size_t)(HEADER_SIZE + originSize)) == 0, "헤더 포함 전송 바이트가 다름");

        // --- 3. 교차 역직렬화: 수정본이 만든 바이트를 원본이 읽는다 ---
        ::CSerialBuffer crossToOrigin(MSG_DEFAULT_SIZE);
        std::memcpy(crossToOrigin.GetWriteBufferPtr(), fixed.GetPayloadBufferPtr(), (size_t)fixedSize);
        TEST_ASSERT(crossToOrigin.MoveWritePos(fixedSize) == fixedSize, "교차 적재 실패(원본측)");

        // --- 4. 반대 방향: 원본이 만든 바이트를 수정본이 읽는다 ---
        Fixed::CSerialBuffer crossToFixed(Fixed::MSG_DEFAULT_SIZE);
        std::memcpy(crossToFixed.GetWriteBufferPtr(), origin.GetPayloadBufferPtr(), (size_t)originSize);
        TEST_ASSERT(crossToFixed.MoveWritePos(originSize) == originSize, "교차 적재 실패(수정본측)");

        for (size_t f = 0; f < fields.size(); ++f)
        {
            const Field& field = fields[f];
            switch (field.type)
            {
            case 0:
            {
                uint8_t a = 0, b = 0; crossToOrigin >> a; crossToFixed >> b;
                TEST_ASSERT(a == (uint8_t)field.raw && b == (uint8_t)field.raw, "교차 역직렬화 uint8_t 불일치");
                break;
            }
            case 1:
            {
                char a = 0, b = 0; crossToOrigin >> a; crossToFixed >> b;
                TEST_ASSERT(a == (char)field.raw && b == (char)field.raw, "교차 역직렬화 char 불일치");
                break;
            }
            case 2:
            {
                short a = 0, b = 0; crossToOrigin >> a; crossToFixed >> b;
                TEST_ASSERT(a == (short)field.raw && b == (short)field.raw, "교차 역직렬화 short 불일치");
                break;
            }
            case 3:
            {
                uint16_t a = 0, b = 0; crossToOrigin >> a; crossToFixed >> b;
                TEST_ASSERT(a == (uint16_t)field.raw && b == (uint16_t)field.raw, "교차 역직렬화 uint16_t 불일치");
                break;
            }
            case 4:
            {
                int a = 0, b = 0; crossToOrigin >> a; crossToFixed >> b;
                TEST_ASSERT(a == (int)field.raw && b == (int)field.raw, "교차 역직렬화 int 불일치");
                break;
            }
            case 5:
            {
                uint32_t a = 0, b = 0; crossToOrigin >> a; crossToFixed >> b;
                TEST_ASSERT(a == (uint32_t)field.raw && b == (uint32_t)field.raw, "교차 역직렬화 uint32_t 불일치");
                break;
            }
            case 6:
            {
                int64_t a = 0, b = 0; crossToOrigin >> a; crossToFixed >> b;
                TEST_ASSERT(a == (int64_t)field.raw && b == (int64_t)field.raw, "교차 역직렬화 int64_t 불일치");
                break;
            }
            case 7:
            {
                uint64_t a = 0, b = 0; crossToOrigin >> a; crossToFixed >> b;
                TEST_ASSERT(a == field.raw && b == field.raw, "교차 역직렬화 uint64_t 불일치");
                break;
            }
            case 8:
            {
                const float expected = (float)(field.raw % 100000) / 7.0f;
                float a = 0.0f, b = 0.0f; crossToOrigin >> a; crossToFixed >> b;
                TEST_ASSERT(std::memcmp(&a, &expected, sizeof(float)) == 0
                    && std::memcmp(&b, &expected, sizeof(float)) == 0, "교차 역직렬화 float 불일치");
                break;
            }
            default:
            {
                const double expected = (double)(field.raw % 1000000) / 13.0;
                double a = 0.0, b = 0.0; crossToOrigin >> a; crossToFixed >> b;
                TEST_ASSERT(std::memcmp(&a, &expected, sizeof(double)) == 0
                    && std::memcmp(&b, &expected, sizeof(double)) == 0, "교차 역직렬화 double 불일치");
                break;
            }
            }
        }

        TEST_ASSERT(crossToOrigin.GetDataSize() == 0 && crossToFixed.GetDataSize() == 0,
            "교차 역직렬화 후 잔여 데이터 존재");

        g_totalIterations++;
        PrintProgress("동등성", round, TestConfig::EQUIVALENCE_ROUNDS);
    }

    // --- 5. 문자열도 길이 접두사까지 동일해야 한다 ---
    {
        const char* narrow = "MMO_Zone SerialBuffer equivalence";
        const wchar_t* wide = L"직렬화 버퍼 동등성";

        ::CSerialBuffer   origin(MSG_DEFAULT_SIZE);
        Fixed::CSerialBuffer fixed(Fixed::MSG_DEFAULT_SIZE);

        origin << narrow << wide;
        fixed << narrow << wide;

        TEST_ASSERT(origin.GetDataSize() == fixed.GetDataSize(), "문자열 직렬화 크기가 다름");
        TEST_ASSERT(std::memcmp(origin.GetPayloadBufferPtr(), fixed.GetPayloadBufferPtr(),
            (size_t)origin.GetDataSize()) == 0, "문자열 직렬화 바이트가 다름");
    }
    std::cout << "  [OK] 문자열(char*/wchar_t*) 길이 접두사까지 동일" << std::endl;

    std::cout << "\n[PASS] 동등성 테스트 완료! (비교한 페이로드 " << comparedBytes << " 바이트)" << std::endl;
    std::cout << "  → 수정본이 만든 패킷을 원본이 그대로 파싱한다 (양방향 확인)" << std::endl;
    g_testCount++;
}

//=============================================================================
// 메뉴 출력
//=============================================================================
void PrintMenu()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "  CSerialBuffer 테스트 메뉴" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "[Phase 1: 직렬화 기본 - 단일 스레드]" << std::endl;
    std::cout << "  1. 전 타입 왕복 테스트" << std::endl;
    std::cout << "  2. 랜덤 혼합 타입 왕복 테스트" << std::endl;
    std::cout << "  3. 문자열 직렬화 테스트" << std::endl;
    std::cout << "  4. 경계 조건 테스트" << std::endl;
    std::cout << "  5. 잘못된 인자 방어 테스트" << std::endl;
    std::cout << "  6. recv 경로 통합 테스트" << std::endl;
    std::cout << "  7. Phase 1 전체 실행" << std::endl;
    std::cout << "\n[Phase 2: 수명 / 봉인]" << std::endl;
    std::cout << "  8. Seal 봉인 테스트" << std::endl;
    std::cout << "  9. 참조 카운팅 기본 테스트" << std::endl;
    std::cout << " 10. Phase 2 전체 실행" << std::endl;
    std::cout << "\n[Phase 3: 멀티스레드 / 실사용 소유권 경로]" << std::endl;
    std::cout << " 11. 브로드캐스트 시나리오 테스트" << std::endl;
    std::cout << " 12. 다중 스레드 수명 스트레스" << std::endl;
    std::cout << " 13. 전송 실패 경로 테스트" << std::endl;
    std::cout << " 14. 타겟 0명 브로드캐스트 테스트" << std::endl;
    std::cout << " 15. 단건+배치 AddRef 혼합 테스트" << std::endl;
    std::cout << " 16. Phase 3 전체 실행" << std::endl;
    std::cout << "\n[Phase 4: 잠재 결함 점검 (비파괴)]" << std::endl;
    std::cout << " 17. 알려진 위험 지점 점검" << std::endl;
    std::cout << "\n[Phase 5: 원본 ↔ 수정본 동등성]" << std::endl;
    std::cout << " 19. 동등성 / 와이어 호환 테스트" << std::endl;
    std::cout << "\n[전체]" << std::endl;
    std::cout << " 20. 전체 실행 (Phase 1 ~ 5)" << std::endl;
    std::cout << "  0. 종료" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "선택: ";
}

//=============================================================================
// Main
//=============================================================================
int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << "  CSerialBuffer 통합 테스트 시스템" << std::endl;
#ifdef USE_FIXED_SERIALBUFFER
    std::cout << "  대상: SerialBuffer_Fixed.{h,cpp}  ← 수정본" << std::endl;
#else
    std::cout << "  대상: SerialBuffer.{h,cpp}  ← MMOServer 원본 사본" << std::endl;
#endif
    std::cout << "  주의: 프리리스트는 테스트용 new/delete 대체본" << std::endl;
    std::cout << "        (락프리 TLS 프리리스트 검증은 LockFree 테스트에서)" << std::endl;
    std::cout << "========================================" << std::endl;

    UTBuffer::_TlsMsgFreeList = new LockFree::CExternalTlsFreeList<UTBuffer>();

    while (true)
    {
        PrintMenu();

        int choice;
        if (!(std::cin >> choice))
            break;

        if (choice == 0)
        {
            std::cout << "\n테스트를 종료합니다." << std::endl;
            break;
        }

        ClearScreen();

        auto totalStart = std::chrono::steady_clock::now();
        g_testCount = 0;
        g_warnCount = 0;

        try
        {
            switch (choice)
            {
            case 1: Test_BasicTypes(); break;
            case 2: Test_RandomMixed(); break;
            case 3: Test_String(); break;
            case 4: Test_Boundary(); break;
            case 5: Test_InvalidArguments(); break;
            case 6: Test_RecvPathIntegration(); break;
            case 7:
                std::cout << "\n[Phase 1 전체 실행]" << std::endl;
                Test_BasicTypes();
                Test_RandomMixed();
                Test_String();
                Test_Boundary();
                Test_InvalidArguments();
                Test_RecvPathIntegration();
                break;
            case 8: Test_Seal(); break;
            case 9: Test_RefCountBasic(); break;
            case 10:
                std::cout << "\n[Phase 2 전체 실행]" << std::endl;
                Test_Seal();
                Test_RefCountBasic();
                break;
            case 11: Test_BroadcastMT(); break;
            case 12: Test_ConcurrentLifecycle(); break;
            case 13: Test_SendFailurePaths(); break;
            case 14: Test_ZeroTargetBroadcast(); break;
            case 15: Test_MixedAddRef(); break;
            case 16:
                std::cout << "\n[Phase 3 전체 실행]" << std::endl;
                Test_BroadcastMT();
                Test_ConcurrentLifecycle();
                Test_SendFailurePaths();
                Test_ZeroTargetBroadcast();
                Test_MixedAddRef();
                break;
            case 17: Test_KnownHazards(); break;
            case 19: Test_Equivalence(); break;
            case 20:
                std::cout << "\n[전체 테스트 실행]" << std::endl;
                Test_BasicTypes();
                Test_RandomMixed();
                Test_String();
                Test_Boundary();
                Test_InvalidArguments();
                Test_RecvPathIntegration();
                Test_Seal();
                Test_RefCountBasic();
                Test_BroadcastMT();
                Test_ConcurrentLifecycle();
                Test_SendFailurePaths();
                Test_ZeroTargetBroadcast();
                Test_MixedAddRef();
                Test_KnownHazards();
                Test_Equivalence();
                break;
            default:
                std::cout << "\n잘못된 선택입니다." << std::endl;
                continue;
            }
        }
        catch (...)
        {
            std::cout << "\n[EXCEPTION] 예외 발생!" << std::endl;
            continue;
        }

        auto totalEnd = std::chrono::steady_clock::now();
        auto totalElapsed = std::chrono::duration_cast<std::chrono::seconds>(totalEnd - totalStart).count();

        std::cout << "\n========================================" << std::endl;
        std::cout << "  테스트 완료!" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "  - 완료된 테스트: " << g_testCount << " 개" << std::endl;
        std::cout << "  - 총 소요 시간: " << totalElapsed << " 초 ("
            << totalElapsed / 60 << " 분)" << std::endl;
        if (g_warnCount.load() > 0)
            std::cout << "  - 결과: [OK] PASS (경고 " << g_warnCount.load() << "건)" << std::endl;
        else
            std::cout << "  - 결과: [OK] 100% PASS" << std::endl;
        std::cout << "========================================" << std::endl;
    }

    delete UTBuffer::_TlsMsgFreeList;
    UTBuffer::_TlsMsgFreeList = nullptr;
    return 0;
}
