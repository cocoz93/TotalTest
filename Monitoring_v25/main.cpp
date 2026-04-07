#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <iphlpapi.h>
#include <string>
#include <vector>
#include <memory>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "psapi.lib")

class GameServerMonitor
{
public:
    // CPU 메트릭 구조체
    struct CPUMetrics
    {
        double totalUsage;                  // 전체 CPU 사용률 (%)
        std::vector<double> perCoreUsage;   // 코어별 사용률 (%)
        double userTime;                    // User Time (%)
        double systemTime;                  // System Time (%)
        DWORD contextSwitches;              // 컨텍스트 스위칭 횟수/초
        double loadAverage[3];              // Load Average (1분, 5분, 15분)
    };

    // Memory 메트릭 구조체
    struct MemoryMetrics
    {
        DWORDLONG usedMemory;       // 사용 중인 메모리 (bytes)
        DWORDLONG freeMemory;       // 사용 가능한 메모리 (bytes)
        DWORDLONG totalMemory;      // 전체 메모리 (bytes)
        DWORDLONG swapUsed;         // 스왑 사용량 (bytes)
        DWORDLONG swapTotal;        // 전체 스왑 크기 (bytes)
        DWORD pageFaults;           // Page Faults/sec
        double memoryUsagePercent;  // 메모리 사용률 (%)
    };

    // Network 메트릭 구조체
    struct NetworkMetrics
    {
        DWORD packetsReceivedPerSec;    // 수신 패킷 수/초 (PPS)
        DWORD packetsSentPerSec;        // 송신 패킷 수/초 (PPS)
        DWORDLONG bytesReceivedPerSec;  // 수신 대역폭 (bytes/sec)
        DWORDLONG bytesSentPerSec;      // 송신 대역폭 (bytes/sec)
        DWORD packetsDropped;           // 패킷 드롭 수
        DWORD packetsErrors;            // 패킷 에러 수
        DWORD tcpBufferUsage;           // TCP 버퍼 사용량
        DWORD udpBufferUsage;           // UDP 버퍼 사용량
    };

    // Disk I/O 메트릭 구조체
    struct DiskMetrics
    {
        double iopsRead;            // 읽기 IOPS
        double iopsWrite;           // 쓰기 IOPS
        double readLatency;         // 읽기 지연시간 (ms)
        double writeLatency;        // 쓰기 지연시간 (ms)
        DWORDLONG diskUsedSpace;    // 사용된 디스크 공간 (bytes)
        DWORDLONG diskFreeSpace;    // 사용 가능한 디스크 공간 (bytes)
        DWORDLONG diskTotalSpace;   // 전체 디스크 공간 (bytes)
        double diskUsagePercent;    // 디스크 사용률 (%)
    };

    GameServerMonitor();
    ~GameServerMonitor();

    bool Initialize();
    void Shutdown();

    // 각 메트릭 수집 함수
    bool CollectCPUMetrics(CPUMetrics& metrics);
    bool CollectMemoryMetrics(MemoryMetrics& metrics);
    bool CollectNetworkMetrics(NetworkMetrics& metrics);
    bool CollectDiskMetrics(DiskMetrics& metrics, const std::wstring& driveLetter = L"C:");

    // 모든 메트릭을 한번에 수집
    bool CollectAllMetrics(CPUMetrics& cpu, MemoryMetrics& memory,
        NetworkMetrics& network, DiskMetrics& disk);

private:
    // PDH 쿼리 핸들
    PDH_HQUERY m_cpuQuery;
    PDH_HQUERY m_memoryQuery;
    PDH_HQUERY m_networkQuery;
    PDH_HQUERY m_diskQuery;

    // PDH 카운터 핸들 - CPU
    PDH_HCOUNTER m_totalCpuCounter;
    std::vector<PDH_HCOUNTER> m_perCoreCpuCounters;
    PDH_HCOUNTER m_userTimeCounter;
    PDH_HCOUNTER m_systemTimeCounter;
    PDH_HCOUNTER m_contextSwitchCounter;

    // PDH 카운터 핸들 - Memory
    PDH_HCOUNTER m_availableMemoryCounter;
    PDH_HCOUNTER m_committedMemoryCounter;
    PDH_HCOUNTER m_pageFaultCounter;

    // PDH 카운터 핸들 - Network
    PDH_HCOUNTER m_packetsRecvCounter;
    PDH_HCOUNTER m_packetsSentCounter;
    PDH_HCOUNTER m_bytesRecvCounter;
    PDH_HCOUNTER m_bytesSentCounter;
    PDH_HCOUNTER m_packetsErrorCounter;

    // PDH 카운터 핸들 - Disk
    PDH_HCOUNTER m_diskReadCounter;
    PDH_HCOUNTER m_diskWriteCounter;
    PDH_HCOUNTER m_diskReadLatencyCounter;
    PDH_HCOUNTER m_diskWriteLatencyCounter;

    // 이전 네트워크 통계 (델타 계산용)
    MIB_IF_ROW2 m_prevNetStats;
    DWORD m_prevNetTimestamp;

    bool m_initialized;
    DWORD m_coreCount;

    // 초기화 헬퍼 함수들
    bool InitializeCPUCounters();
    bool InitializeMemoryCounters();
    bool InitializeNetworkCounters();
    bool InitializeDiskCounters(const std::wstring& driveLetter);

    // 유틸리티 함수
    DWORD GetCoreCount();
    void CalculateLoadAverage(double loadAvg[3]);
};

// 생성자
GameServerMonitor::GameServerMonitor()
    : m_cpuQuery(NULL)
    , m_memoryQuery(NULL)
    , m_networkQuery(NULL)
    , m_diskQuery(NULL)
    , m_totalCpuCounter(NULL)
    , m_userTimeCounter(NULL)
    , m_systemTimeCounter(NULL)
    , m_contextSwitchCounter(NULL)
    , m_availableMemoryCounter(NULL)
    , m_committedMemoryCounter(NULL)
    , m_pageFaultCounter(NULL)
    , m_packetsRecvCounter(NULL)
    , m_packetsSentCounter(NULL)
    , m_bytesRecvCounter(NULL)
    , m_bytesSentCounter(NULL)
    , m_packetsErrorCounter(NULL)
    , m_diskReadCounter(NULL)
    , m_diskWriteCounter(NULL)
    , m_diskReadLatencyCounter(NULL)
    , m_diskWriteLatencyCounter(NULL)
    , m_prevNetTimestamp(0)
    , m_initialized(false)
    , m_coreCount(0)
{
    ZeroMemory(&m_prevNetStats, sizeof(m_prevNetStats));
}

// 소멸자
GameServerMonitor::~GameServerMonitor()
{
    Shutdown();
}

// 초기화
bool GameServerMonitor::Initialize()
{
    if (m_initialized)
        return true;

    m_coreCount = GetCoreCount();

    // PDH 쿼리 생성
    if (PdhOpenQuery(NULL, 0, &m_cpuQuery) != ERROR_SUCCESS)
        return false;
    if (PdhOpenQuery(NULL, 0, &m_memoryQuery) != ERROR_SUCCESS)
        return false;
    if (PdhOpenQuery(NULL, 0, &m_networkQuery) != ERROR_SUCCESS)
        return false;
    if (PdhOpenQuery(NULL, 0, &m_diskQuery) != ERROR_SUCCESS)
        return false;

    // 각 카운터 초기화
    if (!InitializeCPUCounters())
        return false;
    if (!InitializeMemoryCounters())
        return false;
    if (!InitializeNetworkCounters())
        return false;
    if (!InitializeDiskCounters(L"C:"))
        return false;

    // 초기 샘플 수집 (첫 번째 호출은 기준값 설정)
    PdhCollectQueryData(m_cpuQuery);
    PdhCollectQueryData(m_memoryQuery);
    PdhCollectQueryData(m_networkQuery);
    PdhCollectQueryData(m_diskQuery);

    m_initialized = true;
    return true;
}

// 종료
void GameServerMonitor::Shutdown()
{
    if (m_cpuQuery)
    {
        PdhCloseQuery(m_cpuQuery);
        m_cpuQuery = NULL;
    }
    if (m_memoryQuery)
    {
        PdhCloseQuery(m_memoryQuery);
        m_memoryQuery = NULL;
    }
    if (m_networkQuery)
    {
        PdhCloseQuery(m_networkQuery);
        m_networkQuery = NULL;
    }
    if (m_diskQuery)
    {
        PdhCloseQuery(m_diskQuery);
        m_diskQuery = NULL;
    }

    m_perCoreCpuCounters.clear();
    m_initialized = false;
}

// CPU 카운터 초기화
bool GameServerMonitor::InitializeCPUCounters()
{
    PDH_STATUS status;

    // 전체 CPU 사용률
    status = PdhAddCounter(m_cpuQuery, L"\\Processor(_Total)\\% Processor Time",
        0, &m_totalCpuCounter);
    if (status != ERROR_SUCCESS)
        return false;

    // 코어별 CPU 사용률
    m_perCoreCpuCounters.resize(m_coreCount);
    for (DWORD i = 0; i < m_coreCount; i++)
    {
        wchar_t counterPath[256];
        swprintf_s(counterPath, L"\\Processor(%d)\\%% Processor Time", i);
        status = PdhAddCounter(m_cpuQuery, counterPath, 0, &m_perCoreCpuCounters[i]);
        if (status != ERROR_SUCCESS)
            return false;
    }

    // User Time
    status = PdhAddCounter(m_cpuQuery, L"\\Processor(_Total)\\% User Time",
        0, &m_userTimeCounter);
    if (status != ERROR_SUCCESS)
        return false;

    // Privileged Time (System Time)
    status = PdhAddCounter(m_cpuQuery, L"\\Processor(_Total)\\% Privileged Time",
        0, &m_systemTimeCounter);
    if (status != ERROR_SUCCESS)
        return false;

    // Context Switches
    status = PdhAddCounter(m_cpuQuery, L"\\System\\Context Switches/sec",
        0, &m_contextSwitchCounter);
    if (status != ERROR_SUCCESS)
        return false;

    return true;
}

// Memory 카운터 초기화
bool GameServerMonitor::InitializeMemoryCounters()
{
    PDH_STATUS status;

    // Available Memory
    status = PdhAddCounter(m_memoryQuery, L"\\Memory\\Available Bytes",
        0, &m_availableMemoryCounter);
    if (status != ERROR_SUCCESS)
        return false;

    // Committed Memory
    status = PdhAddCounter(m_memoryQuery, L"\\Memory\\Committed Bytes",
        0, &m_committedMemoryCounter);
    if (status != ERROR_SUCCESS)
        return false;

    // Page Faults
    status = PdhAddCounter(m_memoryQuery, L"\\Memory\\Page Faults/sec",
        0, &m_pageFaultCounter);
    if (status != ERROR_SUCCESS)
        return false;

    return true;
}

// Network 카운터 초기화
bool GameServerMonitor::InitializeNetworkCounters()
{
    PDH_STATUS status;

    // 첫 번째 활성 네트워크 인터페이스를 찾아서 사용
    // 실제 환경에서는 특정 인터페이스를 지정하거나 모든 인터페이스를 모니터링할 수 있습니다
    status = PdhAddCounter(m_networkQuery, L"\\Network Interface(*)\\Packets Received/sec",
        0, &m_packetsRecvCounter);
    if (status != ERROR_SUCCESS)
        return false;

    status = PdhAddCounter(m_networkQuery, L"\\Network Interface(*)\\Packets Sent/sec",
        0, &m_packetsSentCounter);
    if (status != ERROR_SUCCESS)
        return false;

    status = PdhAddCounter(m_networkQuery, L"\\Network Interface(*)\\Bytes Received/sec",
        0, &m_bytesRecvCounter);
    if (status != ERROR_SUCCESS)
        return false;

    status = PdhAddCounter(m_networkQuery, L"\\Network Interface(*)\\Bytes Sent/sec",
        0, &m_bytesSentCounter);
    if (status != ERROR_SUCCESS)
        return false;

    status = PdhAddCounter(m_networkQuery, L"\\Network Interface(*)\\Packets Outbound Errors",
        0, &m_packetsErrorCounter);
    if (status != ERROR_SUCCESS)
        return false;

    return true;
}

// Disk 카운터 초기화
bool GameServerMonitor::InitializeDiskCounters(const std::wstring& driveLetter)
{
    PDH_STATUS status;

    // Disk Reads/sec
    status = PdhAddCounter(m_diskQuery, L"\\PhysicalDisk(_Total)\\Disk Reads/sec",
        0, &m_diskReadCounter);
    if (status != ERROR_SUCCESS)
        return false;

    // Disk Writes/sec
    status = PdhAddCounter(m_diskQuery, L"\\PhysicalDisk(_Total)\\Disk Writes/sec",
        0, &m_diskWriteCounter);
    if (status != ERROR_SUCCESS)
        return false;

    // Avg Disk Read Latency
    status = PdhAddCounter(m_diskQuery, L"\\PhysicalDisk(_Total)\\Avg. Disk sec/Read",
        0, &m_diskReadLatencyCounter);
    if (status != ERROR_SUCCESS)
        return false;

    // Avg Disk Write Latency
    status = PdhAddCounter(m_diskQuery, L"\\PhysicalDisk(_Total)\\Avg. Disk sec/Write",
        0, &m_diskWriteLatencyCounter);
    if (status != ERROR_SUCCESS)
        return false;

    return true;
}

// CPU 메트릭 수집
bool GameServerMonitor::CollectCPUMetrics(CPUMetrics& metrics)
{
    if (!m_initialized)
        return false;

    PDH_FMT_COUNTERVALUE counterValue;
    PDH_STATUS status;

    // 데이터 수집
    status = PdhCollectQueryData(m_cpuQuery);
    if (status != ERROR_SUCCESS)
        return false;

    // 전체 CPU 사용률
    status = PdhGetFormattedCounterValue(m_totalCpuCounter, PDH_FMT_DOUBLE,
        NULL, &counterValue);
    if (status == ERROR_SUCCESS)
        metrics.totalUsage = counterValue.doubleValue;

    // 코어별 CPU 사용률
    metrics.perCoreUsage.resize(m_coreCount);
    for (DWORD i = 0; i < m_coreCount; i++)
    {
        status = PdhGetFormattedCounterValue(m_perCoreCpuCounters[i], PDH_FMT_DOUBLE,
            NULL, &counterValue);
        if (status == ERROR_SUCCESS)
            metrics.perCoreUsage[i] = counterValue.doubleValue;
    }

    // User Time
    status = PdhGetFormattedCounterValue(m_userTimeCounter, PDH_FMT_DOUBLE,
        NULL, &counterValue);
    if (status == ERROR_SUCCESS)
        metrics.userTime = counterValue.doubleValue;

    // System Time
    status = PdhGetFormattedCounterValue(m_systemTimeCounter, PDH_FMT_DOUBLE,
        NULL, &counterValue);
    if (status == ERROR_SUCCESS)
        metrics.systemTime = counterValue.doubleValue;

    // Context Switches
    status = PdhGetFormattedCounterValue(m_contextSwitchCounter, PDH_FMT_LONG,
        NULL, &counterValue);
    if (status == ERROR_SUCCESS)
        metrics.contextSwitches = counterValue.longValue;

    // Load Average 계산 (Windows에는 기본 제공되지 않으므로 근사값 사용)
    CalculateLoadAverage(metrics.loadAverage);

    return true;
}

// Memory 메트릭 수집
bool GameServerMonitor::CollectMemoryMetrics(MemoryMetrics& metrics)
{
    if (!m_initialized)
        return false;

    PDH_FMT_COUNTERVALUE counterValue;
    PDH_STATUS status;

    // 데이터 수집
    status = PdhCollectQueryData(m_memoryQuery);
    if (status != ERROR_SUCCESS)
        return false;

    // Available Memory
    status = PdhGetFormattedCounterValue(m_availableMemoryCounter, PDH_FMT_LARGE,
        NULL, &counterValue);
    if (status == ERROR_SUCCESS)
        metrics.freeMemory = counterValue.largeValue;

    // Committed Memory
    status = PdhGetFormattedCounterValue(m_committedMemoryCounter, PDH_FMT_LARGE,
        NULL, &counterValue);
    if (status == ERROR_SUCCESS)
        metrics.usedMemory = counterValue.largeValue;

    // Page Faults
    status = PdhGetFormattedCounterValue(m_pageFaultCounter, PDH_FMT_LONG,
        NULL, &counterValue);
    if (status == ERROR_SUCCESS)
        metrics.pageFaults = counterValue.longValue;

    // GlobalMemoryStatusEx를 사용하여 추가 정보 수집
    MEMORYSTATUSEX memStatus;
    memStatus.dwLength = sizeof(memStatus);
    if (GlobalMemoryStatusEx(&memStatus))
    {
        metrics.totalMemory = memStatus.ullTotalPhys;
        metrics.usedMemory = memStatus.ullTotalPhys - memStatus.ullAvailPhys;
        metrics.freeMemory = memStatus.ullAvailPhys;
        metrics.memoryUsagePercent = static_cast<double>(memStatus.dwMemoryLoad);

        // Swap (Page File) 정보
        metrics.swapTotal = memStatus.ullTotalPageFile - memStatus.ullTotalPhys;
        metrics.swapUsed = (memStatus.ullTotalPageFile - memStatus.ullAvailPageFile) -
            (memStatus.ullTotalPhys - memStatus.ullAvailPhys);
    }

    return true;
}

// Network 메트릭 수집
bool GameServerMonitor::CollectNetworkMetrics(NetworkMetrics& metrics)
{
    if (!m_initialized)
        return false;

    PDH_FMT_COUNTERVALUE counterValue;
    PDH_STATUS status;

    // 데이터 수집
    status = PdhCollectQueryData(m_networkQuery);
    if (status != ERROR_SUCCESS)
        return false;

    // Packets Received/sec
    status = PdhGetFormattedCounterValue(m_packetsRecvCounter, PDH_FMT_LONG,
        NULL, &counterValue);
    if (status == ERROR_SUCCESS)
        metrics.packetsReceivedPerSec = counterValue.longValue;

    // Packets Sent/sec
    status = PdhGetFormattedCounterValue(m_packetsSentCounter, PDH_FMT_LONG,
        NULL, &counterValue);
    if (status == ERROR_SUCCESS)
        metrics.packetsSentPerSec = counterValue.longValue;

    // Bytes Received/sec
    status = PdhGetFormattedCounterValue(m_bytesRecvCounter, PDH_FMT_LARGE,
        NULL, &counterValue);
    if (status == ERROR_SUCCESS)
        metrics.bytesReceivedPerSec = counterValue.largeValue;

    // Bytes Sent/sec
    status = PdhGetFormattedCounterValue(m_bytesSentCounter, PDH_FMT_LARGE,
        NULL, &counterValue);
    if (status == ERROR_SUCCESS)
        metrics.bytesSentPerSec = counterValue.largeValue;

    // Packet Errors
    status = PdhGetFormattedCounterValue(m_packetsErrorCounter, PDH_FMT_LONG,
        NULL, &counterValue);
    if (status == ERROR_SUCCESS)
        metrics.packetsErrors = counterValue.longValue;

    // GetIfEntry2를 사용하여 패킷 드롭 정보 수집
    MIB_IF_ROW2 ifRow;
    ZeroMemory(&ifRow, sizeof(ifRow));
    ifRow.InterfaceIndex = 1; // 첫 번째 인터페이스

    if (GetIfEntry2(&ifRow) == NO_ERROR)
    {
        metrics.packetsDropped = static_cast<DWORD>(ifRow.InDiscards + ifRow.OutDiscards);
    }

    // TCP/UDP 버퍼 사용량은 개별 소켓별로 모니터링해야 하므로
    // 여기서는 시스템 전체 통계를 제공합니다
    metrics.tcpBufferUsage = 0; // 애플리케이션별로 구현 필요
    metrics.udpBufferUsage = 0; // 애플리케이션별로 구현 필요

    return true;
}

// Disk 메트릭 수집
bool GameServerMonitor::CollectDiskMetrics(DiskMetrics& metrics, const std::wstring& driveLetter)
{
    if (!m_initialized)
        return false;

    PDH_FMT_COUNTERVALUE counterValue;
    PDH_STATUS status;

    // 데이터 수집
    status = PdhCollectQueryData(m_diskQuery);
    if (status != ERROR_SUCCESS)
        return false;

    // Disk Reads/sec (IOPS)
    status = PdhGetFormattedCounterValue(m_diskReadCounter, PDH_FMT_DOUBLE,
        NULL, &counterValue);
    if (status == ERROR_SUCCESS)
        metrics.iopsRead = counterValue.doubleValue;

    // Disk Writes/sec (IOPS)
    status = PdhGetFormattedCounterValue(m_diskWriteCounter, PDH_FMT_DOUBLE,
        NULL, &counterValue);
    if (status == ERROR_SUCCESS)
        metrics.iopsWrite = counterValue.doubleValue;

    // Read Latency (초 단위를 밀리초로 변환)
    status = PdhGetFormattedCounterValue(m_diskReadLatencyCounter, PDH_FMT_DOUBLE,
        NULL, &counterValue);
    if (status == ERROR_SUCCESS)
        metrics.readLatency = counterValue.doubleValue * 1000.0;

    // Write Latency (초 단위를 밀리초로 변환)
    status = PdhGetFormattedCounterValue(m_diskWriteLatencyCounter, PDH_FMT_DOUBLE,
        NULL, &counterValue);
    if (status == ERROR_SUCCESS)
        metrics.writeLatency = counterValue.doubleValue * 1000.0;

    // 디스크 공간 정보
    ULARGE_INTEGER freeBytesAvailable;
    ULARGE_INTEGER totalBytes;
    ULARGE_INTEGER totalFreeBytes;

    std::wstring drive = driveLetter + L"\\";
    if (GetDiskFreeSpaceEx(drive.c_str(), &freeBytesAvailable,
        &totalBytes, &totalFreeBytes))
    {
        metrics.diskFreeSpace = freeBytesAvailable.QuadPart;
        metrics.diskTotalSpace = totalBytes.QuadPart;
        metrics.diskUsedSpace = totalBytes.QuadPart - freeBytesAvailable.QuadPart;
        metrics.diskUsagePercent =
            (static_cast<double>(metrics.diskUsedSpace) /
                static_cast<double>(metrics.diskTotalSpace)) * 100.0;
    }

    return true;
}

// 모든 메트릭 한번에 수집
bool GameServerMonitor::CollectAllMetrics(CPUMetrics& cpu, MemoryMetrics& memory,
    NetworkMetrics& network, DiskMetrics& disk)
{
    bool success = true;

    success &= CollectCPUMetrics(cpu);
    success &= CollectMemoryMetrics(memory);
    success &= CollectNetworkMetrics(network);
    success &= CollectDiskMetrics(disk);

    return success;
}

// 코어 수 가져오기
DWORD GameServerMonitor::GetCoreCount()
{
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    return sysInfo.dwNumberOfProcessors;
}

// Load Average 계산 (Windows 근사값)
void GameServerMonitor::CalculateLoadAverage(double loadAvg[3])
{
    // Windows에는 Unix의 load average와 동일한 개념이 없으므로
    // CPU 사용률과 프로세서 큐 길이를 사용하여 근사값 계산
    // 실제 구현 시에는 시간에 따른 CPU 사용률을 누적하여 계산해야 합니다

    PDH_FMT_COUNTERVALUE queueLength;
    PDH_HCOUNTER queueCounter;

    if (PdhAddCounter(m_cpuQuery, L"\\System\\Processor Queue Length",
        0, &queueCounter) == ERROR_SUCCESS)
    {
        PdhCollectQueryData(m_cpuQuery);
        if (PdhGetFormattedCounterValue(queueCounter, PDH_FMT_DOUBLE,
            NULL, &queueLength) == ERROR_SUCCESS)
        {
            // 간단한 근사값 (실제로는 시간 가중 평균 필요)
            loadAvg[0] = queueLength.doubleValue; // 1분
            loadAvg[1] = queueLength.doubleValue; // 5분
            loadAvg[2] = queueLength.doubleValue; // 15분
        }
        PdhRemoveCounter(queueCounter);
    }
}

// 사용 예제 (main 함수 또는 다른 곳에서 호출)
/*
int main()
{
    GameServerMonitor monitor;

    if (!monitor.Initialize())
    {
        printf("모니터 초기화 실패\n");
        return -1;
    }

    GameServerMonitor::CPUMetrics cpu;
    GameServerMonitor::MemoryMetrics memory;
    GameServerMonitor::NetworkMetrics network;
    GameServerMonitor::DiskMetrics disk;

    // 1초마다 메트릭 수집
    for (int i = 0; i < 10; i++)
    {
        Sleep(1000);

        if (monitor.CollectAllMetrics(cpu, memory, network, disk))
        {
            printf("=== CPU ===\n");
            printf("전체 사용률: %.2f%%\n", cpu.totalUsage);
            printf("User Time: %.2f%%, System Time: %.2f%%\n",
                   cpu.userTime, cpu.systemTime);
            printf("Context Switches: %u/sec\n", cpu.contextSwitches);

            printf("\n=== Memory ===\n");
            printf("사용: %llu MB / 전체: %llu MB (%.2f%%)\n",
                   memory.usedMemory / (1024*1024),
                   memory.totalMemory / (1024*1024),
                   memory.memoryUsagePercent);
            printf("Swap 사용: %llu MB\n", memory.swapUsed / (1024*1024));
            printf("Page Faults: %u/sec\n", memory.pageFaults);

            printf("\n=== Network ===\n");
            printf("PPS - Recv: %u, Send: %u\n",
                   network.packetsReceivedPerSec, network.packetsSentPerSec);
            printf("대역폭 - Recv: %llu KB/s, Send: %llu KB/s\n",
                   network.bytesReceivedPerSec / 1024,
                   network.bytesSentPerSec / 1024);
            printf("Packet Drop: %u, Errors: %u\n",
                   network.packetsDropped, network.packetsErrors);

            printf("\n=== Disk ===\n");
            printf("IOPS - Read: %.2f, Write: %.2f\n",
                   disk.iopsRead, disk.iopsWrite);
            printf("Latency - Read: %.2f ms, Write: %.2f ms\n",
                   disk.readLatency, disk.writeLatency);
            printf("디스크 사용: %llu GB / %llu GB (%.2f%%)\n",
                   disk.diskUsedSpace / (1024*1024*1024),
                   disk.diskTotalSpace / (1024*1024*1024),
                   disk.diskUsagePercent);
            printf("\n");
        }
    }

    monitor.Shutdown();
    return 0;
}
*/