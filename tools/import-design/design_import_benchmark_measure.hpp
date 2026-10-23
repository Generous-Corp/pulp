#pragma once

// Process and host readings for the design-import benchmark.
//
// Every reading is a value or an explicit absence: a failed or unsupported
// probe yields `std::nullopt` with the reason, never a 0 that a summary would
// average in as a real measurement. Host conditions (load average and Apple
// thermal state) are sampled at phase boundaries so a receipt taken on a
// contended or throttled machine says so and cannot be cited as a speed win.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pulp::design_import_bench {

struct Reading {
    std::optional<std::uint64_t> value;
    std::string source;
    // Why `value` is absent; empty when it is present.
    std::string reason;
};

// Resident set size now.
Reading rss_reading();
// Physical footprint now (TASK_VM_INFO.phys_footprint): the memory the kernel
// charges this process, including compressed and purgeable-but-dirty pages,
// which the resident set undercounts. Apple only.
Reading phys_footprint_reading();
// Resident-set high-water mark since process start; unlike a sampled RSS it
// cannot miss a transient peak between frames.
Reading rss_high_water_reading();

// Summary of one reading taken once per sampled frame. Any absent sample makes
// the whole summary absent, with the first reason seen.
struct MemoryStats {
    std::optional<std::uint64_t> median_bytes;
    std::optional<std::uint64_t> p99_bytes;
    std::optional<std::uint64_t> peak_bytes;
    std::string source;
    std::string reason;
};

class MemorySeries {
  public:
    void add(const Reading& reading);
    MemoryStats summary() const;
    bool empty() const {
        return values_.empty() && reason_.empty();
    }

  private:
    std::vector<double> values_;
    std::string source_;
    std::string reason_;
};

// One observation of the machine the benchmark runs on.
struct HostSample {
    std::optional<double> load_1m;
    std::string load_reason;
    // nominal | fair | serious | critical | unknown, as Foundation reports it;
    // empty when the platform has no thermal-state source.
    std::string thermal;
    std::string thermal_reason;
};

// Samples load and thermal state. A planted load replaces the measured
// one-minute load average, so the confounder classification can be driven to
// its refusing branch on demand.
HostSample sample_host(std::optional<double> planted_load = std::nullopt);

unsigned logical_cores();

// A receipt is confounded when any sample's one-minute load exceeds a quarter
// of the logical cores, a load or thermal reading is unavailable, or the
// thermal state is anything but nominal. A confounded receipt is diagnostic
// only; it can never be cited as a speed win.
struct HostConditions {
    unsigned cores = 0;
    double load_threshold = 0.0;
    bool confounded = false;
    std::vector<std::string> confounders;
};
HostConditions classify_host(const std::vector<HostSample>& samples, unsigned cores);

// Linearly interpolated percentile (0-100); 0 for an empty series.
double percentile(std::vector<double> values, double pct);

// Least-squares slope of `ys` against their index; absent below two points.
std::optional<double> least_squares_slope(const std::vector<double>& ys);

} // namespace pulp::design_import_bench
