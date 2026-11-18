#pragma once

// Pure /proc/meminfo classification for the Linux environment adapter. It has
// no platform dependency so the thresholds can be tested on any host.

#include <pulp/platform/environment.hpp>

#include <cerrno>
#include <cstdlib>
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
    if (line.compare(0, key.size(), key) != 0)
        return field;
    // "<key> <digits> kB" with only whitespace after the unit; any other unit,
    // a missing unit, or trailing text leaves the field absent.
    const char* p = line.c_str() + key.size();
    while (*p == ' ' || *p == '\t')
        ++p;
    if (*p < '0' || *p > '9')
        return field;
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(p, &end, 10);
    if (errno != 0 || end == p || value < 0)
        return field;
    p = end;
    if (*p != ' ' && *p != '\t')
        return field;
    while (*p == ' ' || *p == '\t')
        ++p;
    if (p[0] != 'k' || p[1] != 'B')
        return field;
    p += 2;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        ++p;
    if (*p != '\0')
        return field;
    field.present = true;
    field.kb = value;
    return field;
}

// Critical below 5% of MemTotal available, moderate below 15%. A missing or
// unparsable MemTotal, or a missing MemAvailable, is "no reading" and reports
// normal; a parsed MemAvailable of zero is the most critical reading.
inline MemoryPressure classify_memory_pressure(const MeminfoField& total,
                                               const MeminfoField& available) {
    if (!total.present || total.kb <= 0 || !available.present)
        return MemoryPressure::normal;
    const double frac = static_cast<double>(available.kb) / static_cast<double>(total.kb);
    if (frac < 0.05)
        return MemoryPressure::critical;
    if (frac < 0.15)
        return MemoryPressure::moderate;
    return MemoryPressure::normal;
}

} // namespace pulp::platform::linux_env
