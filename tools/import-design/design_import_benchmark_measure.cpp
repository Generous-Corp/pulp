#include "tools/import-design/design_import_benchmark_measure.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <thread>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <fstream>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace pulp::design_import_bench {

namespace {

Reading absent(std::string source, std::string reason) {
    return {std::nullopt, std::move(source), std::move(reason)};
}

std::string format_load(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << value;
    return out.str();
}

} // namespace

Reading rss_reading() {
#if defined(__APPLE__)
    const char* source = "mach_task_basic_info.resident_size";
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                  &count) != KERN_SUCCESS)
        return absent(source, "task_info(MACH_TASK_BASIC_INFO) failed");
    return {static_cast<std::uint64_t>(info.resident_size), source, {}};
#elif defined(__linux__)
    const char* source = "/proc/self/statm.resident";
    std::ifstream statm("/proc/self/statm");
    std::uint64_t pages = 0;
    std::uint64_t resident = 0;
    if (!(statm >> pages >> resident))
        return absent(source, "could not read /proc/self/statm");
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        return absent(source, "sysconf(_SC_PAGESIZE) failed");
    return {resident * static_cast<std::uint64_t>(page_size), source, {}};
#else
    return absent("none", "no resident-set probe on this platform");
#endif
}

Reading phys_footprint_reading() {
#if defined(__APPLE__)
    const char* source = "task_vm_info.phys_footprint";
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
        KERN_SUCCESS)
        return absent(source, "task_info(TASK_VM_INFO) failed");
    return {static_cast<std::uint64_t>(info.phys_footprint), source, {}};
#else
    return absent("none", "phys_footprint is an Apple task_vm_info field");
#endif
}

Reading rss_high_water_reading() {
#if defined(__APPLE__) || defined(__linux__)
    const char* source = "getrusage.ru_maxrss";
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        return absent(source, "getrusage(RUSAGE_SELF) failed");
#if defined(__APPLE__)
    return {static_cast<std::uint64_t>(usage.ru_maxrss), source, {}};
#else
    return {static_cast<std::uint64_t>(usage.ru_maxrss) * 1024u, source, {}};
#endif
#else
    return absent("none", "no high-water probe on this platform");
#endif
}

void MemorySeries::add(const Reading& reading) {
    if (source_.empty())
        source_ = reading.source;
    if (!reading.value) {
        if (reason_.empty())
            reason_ = reading.reason.empty() ? "reading unavailable" : reading.reason;
        return;
    }
    values_.push_back(static_cast<double>(*reading.value));
}

MemoryStats MemorySeries::summary() const {
    MemoryStats stats;
    stats.source = source_;
    if (!reason_.empty()) {
        stats.reason = reason_;
        return stats;
    }
    if (values_.empty()) {
        stats.reason = "no sampled frames";
        return stats;
    }
    stats.median_bytes = static_cast<std::uint64_t>(std::llround(percentile(values_, 50.0)));
    stats.p99_bytes = static_cast<std::uint64_t>(std::llround(percentile(values_, 99.0)));
    stats.peak_bytes =
        static_cast<std::uint64_t>(*std::max_element(values_.begin(), values_.end()));
    return stats;
}

HostSample sample_host(std::optional<double> planted_load) {
    HostSample sample;
    if (planted_load) {
        sample.load_1m = planted_load;
    } else {
#if defined(__APPLE__) || defined(__linux__)
        double loads[1] = {0.0};
        if (getloadavg(loads, 1) == 1)
            sample.load_1m = loads[0];
        else
            sample.load_reason = "getloadavg failed";
#else
        sample.load_reason = "no load-average probe on this platform";
#endif
    }
#if defined(__APPLE__)
    // The Foundation call tools/scripts/p2_thermal_state.swift makes, reached
    // through the Objective-C runtime so this C++ tool needs no Swift or
    // Objective-C++ translation unit.
    auto* process_info_class = objc_getClass("NSProcessInfo");
    if (!process_info_class) {
        sample.thermal_reason = "Foundation is not loaded";
        return sample;
    }
    using GetObject = id (*)(id, SEL);
    using GetInteger = long (*)(id, SEL);
    id process_info = reinterpret_cast<GetObject>(objc_msgSend)(
        reinterpret_cast<id>(process_info_class), sel_registerName("processInfo"));
    if (!process_info) {
        sample.thermal_reason = "NSProcessInfo.processInfo returned nil";
        return sample;
    }
    switch (reinterpret_cast<GetInteger>(objc_msgSend)(process_info,
                                                       sel_registerName("thermalState"))) {
    case 0:
        sample.thermal = "nominal";
        break;
    case 1:
        sample.thermal = "fair";
        break;
    case 2:
        sample.thermal = "serious";
        break;
    case 3:
        sample.thermal = "critical";
        break;
    default:
        sample.thermal = "unknown";
        break;
    }
#else
    sample.thermal_reason = "no thermal-state source on this platform";
#endif
    return sample;
}

unsigned logical_cores() {
#if defined(__APPLE__)
    int cores = 0;
    std::size_t size = sizeof(cores);
    if (sysctlbyname("hw.logicalcpu", &cores, &size, nullptr, 0) == 0 && cores > 0)
        return static_cast<unsigned>(cores);
#endif
    return std::max(1u, std::thread::hardware_concurrency());
}

HostConditions classify_host(const std::vector<HostSample>& samples, unsigned cores) {
    HostConditions conditions;
    conditions.cores = std::max(1u, cores);
    conditions.load_threshold = static_cast<double>(conditions.cores) / 4.0;
    const auto add = [&](std::string reason) {
        if (std::find(conditions.confounders.begin(), conditions.confounders.end(), reason) ==
            conditions.confounders.end())
            conditions.confounders.push_back(std::move(reason));
    };
    if (samples.empty())
        add("no host samples");
    for (const auto& sample : samples) {
        if (!sample.load_1m)
            add("load unavailable: " + sample.load_reason);
        else if (*sample.load_1m > conditions.load_threshold)
            add("load_1m " + format_load(*sample.load_1m) + " > cores/4 (" +
                format_load(conditions.load_threshold) + ")");
        if (sample.thermal.empty())
            add("thermal state unavailable: " + sample.thermal_reason);
        else if (sample.thermal != "nominal")
            add("thermal state " + sample.thermal);
    }
    conditions.confounded = !conditions.confounders.empty();
    return conditions;
}

double percentile(std::vector<double> values, double pct) {
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const double rank = (pct / 100.0) * static_cast<double>(values.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(rank));
    const auto hi = static_cast<std::size_t>(std::ceil(rank));
    if (lo == hi)
        return values[lo];
    const double w = rank - static_cast<double>(lo);
    return values[lo] * (1.0 - w) + values[hi] * w;
}

std::optional<double> least_squares_slope(const std::vector<double>& ys) {
    if (ys.size() < 2)
        return std::nullopt;
    const double n = static_cast<double>(ys.size());
    const double mean_x = (n - 1.0) / 2.0;
    double mean_y = 0.0;
    for (double y : ys)
        mean_y += y;
    mean_y /= n;
    double sxy = 0.0;
    double sxx = 0.0;
    for (std::size_t i = 0; i < ys.size(); ++i) {
        const double dx = static_cast<double>(i) - mean_x;
        sxy += dx * (ys[i] - mean_y);
        sxx += dx * dx;
    }
    return sxy / sxx;
}

} // namespace pulp::design_import_bench
