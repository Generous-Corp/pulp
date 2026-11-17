#pragma once

// Pure /proc/meminfo classification for the Linux environment adapter. It has
// no platform dependency so the thresholds can be tested on any host.

#include <pulp/platform/environment.hpp>

#include <cstdio>
#include <string>
#include <string_view>

namespace pulp::platform::linux_env {

// One /proc/meminfo field: whether the line was present and parsed, and its
// value in kB. A parsed zero is a real reading, distinct from an absent field.
struct MeminfoField {
    bool present = false;
    long kb = 0;
};

inline MeminfoField parse_meminfo_line(const std::string& line, std::string_view key) {
    MeminfoField field;
    if (line.compare(0, key.size(), key) != 0) return field;
    long value = 0;
    const std::string format = std::string(key) + " %ld kB";
    if (std::sscanf(line.c_str(), format.c_str(), &value) == 1 && value >= 0) {
        field.present = true;
        field.kb = value;
    }
    return field;
}

// Critical below 5% of MemTotal available, moderate below 15%. A missing or
// unparsable MemTotal, or a missing MemAvailable, is "no reading" and reports
// normal; a parsed MemAvailable of zero is the most critical reading.
inline MemoryPressure classify_memory_pressure(const MeminfoField& total,
                                               const MeminfoField& available) {
    if (!total.present || total.kb <= 0 || !available.present) return MemoryPressure::normal;
    const double frac = static_cast<double>(available.kb) / static_cast<double>(total.kb);
    if (frac < 0.05) return MemoryPressure::critical;
    if (frac < 0.15) return MemoryPressure::moderate;
    return MemoryPressure::normal;
}

}  // namespace pulp::platform::linux_env
