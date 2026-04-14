//

#include <iostream>
#include <algorithm>
#include <iomanip>
#include "Profiler.h"

// ============================================================================
// TLS 기반 고성능 멀티스레드 프로파일러 구현
// ============================================================================

namespace Profiler
{
    // ProfileData 생성자
    ProfileData::ProfileData()
        : name(nullptr), totalTime(0), callCount(0),
        minTime(UINT64_MAX), maxTime(0)
    {
    }

    // CThreadLocalProfiler 생성자
    CThreadLocalProfiler::CThreadLocalProfiler()
        : _profileCount(0)
    {
    }

    size_t CThreadLocalProfiler::GetOrCreateIndex(const char* name)
    {
        auto it = _nameToIndex.find(name);
        if (it != _nameToIndex.end())
        {
            return it->second;
        }

        if (_profileCount >= MAX_PROFILES)
        {
            return SIZE_MAX; // 오버플로우 방지
        }

        size_t index = _profileCount++;
        _profiles[index].name = name;
        _nameToIndex[name] = index;
        return index;
    }

    void CThreadLocalProfiler::Record(size_t index, uint64_t elapsed)
    {
        if (index >= MAX_PROFILES) return;

        auto& data = _profiles[index];
        data.totalTime += elapsed;
        data.callCount++;
        data.minTime = (std::min)(data.minTime, elapsed);
        data.maxTime = (std::max)(data.maxTime, elapsed);
    }

    const ProfileData* CThreadLocalProfiler::GetProfiles() const
    {
        return _profiles;
    }

    size_t CThreadLocalProfiler::GetProfileCount() const
    {
        return _profileCount;
    }

    // CProfilerManager 생성자
    CProfilerManager::CProfilerManager()
        : _enabled(true)
    {
    }

    CProfilerManager& CProfilerManager::Instance()
    {
        static CProfilerManager instance;
        return instance;
    }

    void CProfilerManager::RegisterThreadProfiler(CThreadLocalProfiler* profiler)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _threadProfilers.push_back(profiler);
    }

    void CProfilerManager::UnregisterThreadProfiler(CThreadLocalProfiler* profiler)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = std::find(_threadProfilers.begin(), _threadProfilers.end(), profiler);
        if (it != _threadProfilers.end())
        {
            _threadProfilers.erase(it);
        }
    }

    bool CProfilerManager::IsEnabled() const
    {
        return _enabled.load(std::memory_order_relaxed);
    }

    void CProfilerManager::SetEnabled(bool enabled)
    {
        _enabled.store(enabled, std::memory_order_relaxed);
    }

    // 모든 스레드의 프로파일 데이터 집계 및 출력
    void CProfilerManager::PrintReport()
    {
        std::lock_guard<std::mutex> lock(_mutex);

        std::unordered_map<const char*, ProfileData> aggregated;

        for (auto* profiler : _threadProfilers)
        {
            const auto* profiles = profiler->GetProfiles();
            size_t count = profiler->GetProfileCount();

            for (size_t i = 0; i < count; ++i)
            {
                const auto& src = profiles[i];
                auto& dst = aggregated[src.name];

                if (dst.name == nullptr)
                {
                    dst.name = src.name;
                    dst.minTime = src.minTime;
                }

                dst.totalTime += src.totalTime;
                dst.callCount += src.callCount;
                dst.minTime = (std::min)(dst.minTime, src.minTime);
                dst.maxTime = (std::max)(dst.maxTime, src.maxTime);
            }
        }

        // 결과 출력
        std::cout << "\n========== PROFILER REPORT ==========\n";
        std::cout << std::left << std::setw(30) << "Name"
            << std::right << std::setw(12) << "Calls"
            << std::setw(15) << "Total(ms)"
            << std::setw(12) << "Avg(us)"
            << std::setw(12) << "Min(us)"
            << std::setw(12) << "Max(us)" << "\n";
        std::cout << std::string(93, '-') << "\n";

        for (const auto& pair : aggregated)
        {
            const auto& data = pair.second;
            double totalMs = data.totalTime / 1'000'000.0;
            double avgUs = data.callCount > 0 ? (data.totalTime / 1000.0) / data.callCount : 0;
            double minUs = data.minTime / 1000.0;
            double maxUs = data.maxTime / 1000.0;

            std::cout << std::left << std::setw(30) << data.name
                << std::right << std::setw(12) << data.callCount
                << std::setw(15) << std::fixed << std::setprecision(3) << totalMs
                << std::setw(12) << std::setprecision(2) << avgUs
                << std::setw(12) << minUs
                << std::setw(12) << maxUs << "\n";
        }
        std::cout << "======================================\n\n";
    }

    // TLS 인스턴스 관리
    CThreadLocalProfiler& GetThreadLocalProfiler()
    {
        thread_local struct ProfilerGuard
        {
            CThreadLocalProfiler profiler;

            ProfilerGuard()
            {
                CProfilerManager::Instance().RegisterThreadProfiler(&profiler);
            }

            ~ProfilerGuard()
            {
                CProfilerManager::Instance().UnregisterThreadProfiler(&profiler);
            }
        } guard;

        return guard.profiler;
    }

    // CScopedProfiler 생성자
    CScopedProfiler::CScopedProfiler(const char* name)
        : _profiler(GetThreadLocalProfiler())
        , _enabled(CProfilerManager::Instance().IsEnabled())
    {
        if (_enabled)
        {
            _index = _profiler.GetOrCreateIndex(name);
            _start = std::chrono::high_resolution_clock::now();
        }
    }

    // CScopedProfiler 소멸자
    CScopedProfiler::~CScopedProfiler()
    {
        if (_enabled)
        {
            auto end = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - _start).count();
            _profiler.Record(_index, static_cast<uint64_t>(elapsed));
        }
    }

} // namespace Profiler

// 편의 매크로
#define PROFILE_SCOPE(name) Profiler::CScopedProfiler _profiler_##__LINE__(name)
#define PROFILE_FUNCTION() PROFILE_SCOPE(__FUNCTION__)