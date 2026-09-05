//
#pragma once

//=============================================================================
// [테스트 전용 대체 헤더 — 원본이 아님]
//
// 원본 MMOServer/MMOServer/Platform/Platform.h 는 470줄짜리 플랫폼 추상화
// (windows.h / 소켓 / 스레드 유틸)다. SerialBuffer.cpp 가 실제로 쓰는 것은
// memcpy_s(6회) · wcslen(2회) · strlen(1회) 뿐이므로 그만큼만 제공한다.
//
// MSVC 에서는 셋 다 CRT 에 있어 이 헤더는 사실상 <string.h> 포함일 뿐이고,
// 비-MSVC(리눅스 g++ 등)에서 테스트를 돌릴 때만 memcpy_s 를 채워 넣는다.
//=============================================================================

#include <cstring>
#include <cwchar>
#include <cstddef>

#ifndef _MSC_VER
#include <cerrno>

// C11 Annex K 의 memcpy_s 축약 구현 — SerialBuffer.cpp 가 쓰는 형태만 만족시킨다.
// 원본과 동일하게 "대상 공간이 모자라면 복사하지 않고 실패"로 동작한다.
inline int memcpy_s(void* dest, size_t destSize, const void* src, size_t count)
{
    if (count == 0)
        return 0;

    if (dest == nullptr)
        return EINVAL;

    if (src == nullptr || destSize < count)
    {
        std::memset(dest, 0, destSize);
        return src == nullptr ? EINVAL : ERANGE;
    }

    std::memcpy(dest, src, count);
    return 0;
}
#endif
