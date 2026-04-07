#include <pulp/runtime/system.hpp>
#include <thread>

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/utsname.h>
#elif defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <fstream>
#endif

namespace pulp::runtime {

static SystemInfo query_system_info() {
    SystemInfo info;

#ifdef __APPLE__
    info.os_name = "macOS";

    // OS version
    char version[64] = {};
    size_t version_len = sizeof(version);
    if (sysctlbyname("kern.osproductversion", version, &version_len, nullptr, 0) == 0)
        info.os_version = version;

    // CPU model
    char model[256] = {};
    size_t model_len = sizeof(model);
    if (sysctlbyname("machdep.cpu.brand_string", model, &model_len, nullptr, 0) == 0)
        info.cpu_model = model;

    // CPU cores
    int cores = 0;
    size_t cores_len = sizeof(cores);
    if (sysctlbyname("hw.physicalcpu", &cores, &cores_len, nullptr, 0) == 0)
        info.cpu_cores = cores;

    info.cpu_threads = static_cast<int>(std::thread::hardware_concurrency());

    // Memory
    int64_t mem = 0;
    size_t mem_len = sizeof(mem);
    if (sysctlbyname("hw.memsize", &mem, &mem_len, nullptr, 0) == 0)
        info.total_memory_mb = static_cast<uint64_t>(mem) / (1024 * 1024);

    // Architecture
#if defined(__aarch64__) || defined(__arm64__)
    info.arch = "arm64";
#else
    info.arch = "x86_64";
#endif

#elif defined(_WIN32)
    info.os_name = "Windows";

    OSVERSIONINFOEXA osvi{};
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    // Note: GetVersionEx is deprecated but works for basic version info
    info.os_version = std::to_string(osvi.dwMajorVersion) + "." +
                      std::to_string(osvi.dwMinorVersion) + "." +
                      std::to_string(osvi.dwBuildNumber);

    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    info.cpu_threads = static_cast<int>(si.dwNumberOfProcessors);
    info.cpu_cores = info.cpu_threads;  // Simplified

    MEMORYSTATUSEX memstat;
    memstat.dwLength = sizeof(memstat);
    if (GlobalMemoryStatusEx(&memstat))
        info.total_memory_mb = memstat.ullTotalPhys / (1024 * 1024);

    switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_ARM64: info.arch = "arm64"; break;
        case PROCESSOR_ARCHITECTURE_AMD64: info.arch = "x86_64"; break;
        case PROCESSOR_ARCHITECTURE_INTEL: info.arch = "x86"; break;
        default: info.arch = "unknown"; break;
    }

#elif defined(__linux__)
    info.os_name = "Linux";

    struct utsname uts;
    if (uname(&uts) == 0) {
        info.os_version = uts.release;
        info.arch = uts.machine;
    }

    // CPU model from /proc/cpuinfo
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.find("model name") != std::string::npos) {
            auto pos = line.find(':');
            if (pos != std::string::npos) {
                info.cpu_model = line.substr(pos + 2);
                break;
            }
        }
    }

    info.cpu_threads = static_cast<int>(std::thread::hardware_concurrency());
    info.cpu_cores = info.cpu_threads;  // Simplified

    struct sysinfo si;
    if (sysinfo(&si) == 0)
        info.total_memory_mb = (si.totalram * si.mem_unit) / (1024 * 1024);
#endif

    // CPU features — runtime detection (not compile-time)
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    // x86: detect via CPUID at runtime
    {
        #ifdef _MSC_VER
        int cpuinfo[4] = {};
        __cpuid(cpuinfo, 1);
        int ecx1 = cpuinfo[2];
        int edx1 = cpuinfo[3];
        __cpuid(cpuinfo, 7);
        int ebx7 = cpuinfo[1];
        #else
        unsigned int eax, ebx, ecx, edx;
        __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
        int ecx1 = static_cast<int>(ecx);
        int edx1 = static_cast<int>(edx);
        __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7), "c"(0));
        int ebx7 = static_cast<int>(ebx);
        #endif

        info.has_sse2    = (edx1 >> 26) & 1;
        info.has_sse4_1  = (ecx1 >> 19) & 1;
        info.has_avx     = (ecx1 >> 28) & 1;
        info.has_fma     = (ecx1 >> 12) & 1;
        info.has_avx2    = (ebx7 >>  5) & 1;
        info.has_avx512  = (ebx7 >> 16) & 1;
    }
#elif defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    info.has_neon = true;  // Always available on arm64
#endif

    return info;
}

const SystemInfo& get_system_info() {
    static const SystemInfo info = query_system_info();
    return info;
}

int cpu_thread_count() {
    return get_system_info().cpu_threads;
}

uint64_t total_memory_mb() {
    return get_system_info().total_memory_mb;
}

}  // namespace pulp::runtime
