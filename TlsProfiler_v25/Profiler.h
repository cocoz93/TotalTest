#pragma once

#include <chrono>
#include <atomic>
#include <mutex>
#include <vector>
#include <unordered_map>

// ============================================================================
// TLS 기반 고성능 멀티스레드 프로파일러
// ============================================================================
namespace Profiler
{
    // 프로파일 측정 결과 데이터
    struct ProfileData
    {
        const char* name;
        uint64_t totalTime;      // 나노초 단위
        uint64_t callCount;
        uint64_t minTime;
        uint64_t maxTime;

        ProfileData();
    };

    // TLS 프로파일 저장소 (스레드별 독립 저장소 - Lock-free)
    class CThreadLocalProfiler
    {
    public:
        static constexpr size_t MAX_PROFILES = 256;

    private:
        ProfileData _profiles[MAX_PROFILES];
        size_t _profileCount;
        std::unordered_map<const char*, size_t> _nameToIndex;

    public:
        CThreadLocalProfiler();
        size_t GetOrCreateIndex(const char* name);
        void Record(size_t index, uint64_t elapsed);
        const ProfileData* GetProfiles() const;
        size_t GetProfileCount() const;
    };

    // 전역 프로파일러 관리자
    class CProfilerManager
    {
    private:
        std::vector<CThreadLocalProfiler*> _threadProfilers;
        std::mutex _mutex;
        std::atomic<bool> _enabled;

        CProfilerManager();

    public:
        static CProfilerManager& Instance();

        void RegisterThreadProfiler(CThreadLocalProfiler* profiler);
        void UnregisterThreadProfiler(CThreadLocalProfiler* profiler);
        bool IsEnabled() const;
        void SetEnabled(bool enabled);
        void PrintReport();
    };

    // TLS 인스턴스 관리
    CThreadLocalProfiler& GetThreadLocalProfiler();

    // RAII 스코프 타이머
    class CScopedProfiler
    {
    private:
        size_t _index;
        std::chrono::high_resolution_clock::time_point _start;
        CThreadLocalProfiler& _profiler;
        bool _enabled;

    public:
        explicit CScopedProfiler(const char* name);
        ~CScopedProfiler();

        // 복사/이동 방지
        CScopedProfiler(const CScopedProfiler&) = delete;
        CScopedProfiler& operator=(const CScopedProfiler&) = delete;
    };

} // namespace Profiler

// 편의 매크로
#define PROFILE_SCOPE(name) Profiler::CScopedProfiler _profiler_##__LINE__(name)
