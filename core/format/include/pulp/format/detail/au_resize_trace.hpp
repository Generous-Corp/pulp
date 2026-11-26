#pragma once

// Opt-in AU editor geometry trace.
//
// Off unless PULP_AUV2_RESIZE_TRACE is set (not "0") in the plug-in's process
// or the touch file <home>/Library/Logs/Pulp/au-resize-trace.enabled exists.
// The file switch reaches a plug-in hosted out of process (Logic's
// AUHostingService), where a shell variable never arrives. Lines go to stderr
// and are appended, timestamped, to <home>/Library/Logs/Pulp/au-resize-trace.log;
// past kAuResizeTraceMaxBytes the log rolls to au-resize-trace.log.1.
//
// The decision is made once per process. When off, a call site costs one
// cached bool test: PULP_AU_RESIZE_TRACE formats nothing.

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <optional>
#include <string>

#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pulp::format::detail {

inline constexpr long kAuResizeTraceMaxBytes = 1L << 20;

struct AuResizeTraceConfig {
    bool enabled = false;
    std::string log_path; // empty: stderr only
};

/// The user's real home, also inside a sandboxed host process whose HOME
/// points at a container.
inline std::string au_resize_trace_home() {
    if (const passwd* pw = ::getpwuid(::getuid()); pw != nullptr && pw->pw_dir != nullptr)
        return pw->pw_dir;
    const char* home = std::getenv("HOME");
    return home != nullptr ? home : "";
}

/// Pure decision: `env_value` is PULP_AUV2_RESIZE_TRACE (or null), `home` the
/// directory holding Library/Logs/Pulp.
inline AuResizeTraceConfig resolve_au_resize_trace(const char* env_value, const std::string& home) {
    AuResizeTraceConfig config;
    const bool env_on = env_value != nullptr && env_value[0] != '\0' && env_value[0] != '0';
    std::string dir;
    bool file_on = false;
    if (!home.empty()) {
        dir = home + "/Library/Logs/Pulp";
        struct stat st{};
        file_on = ::stat((dir + "/au-resize-trace.enabled").c_str(), &st) == 0;
    }
    config.enabled = env_on || file_on;
    if (config.enabled && !dir.empty()) {
        ::mkdir((home + "/Library").c_str(), 0755);
        ::mkdir((home + "/Library/Logs").c_str(), 0755);
        ::mkdir(dir.c_str(), 0755);
        config.log_path = dir + "/au-resize-trace.log";
    }
    return config;
}

inline std::optional<AuResizeTraceConfig>& au_resize_trace_override() {
    static std::optional<AuResizeTraceConfig> value;
    return value;
}

inline const AuResizeTraceConfig& au_resize_trace_config() {
    static const AuResizeTraceConfig config =
        resolve_au_resize_trace(std::getenv("PULP_AUV2_RESIZE_TRACE"), au_resize_trace_home());
    if (const auto& forced = au_resize_trace_override())
        return *forced;
    return config;
}

inline bool au_resize_trace_enabled() {
    return au_resize_trace_config().enabled;
}

/// Test seam: force the configuration; nullopt restores the process decision.
inline void set_au_resize_trace_for_testing(std::optional<AuResizeTraceConfig> config) {
    au_resize_trace_override() = std::move(config);
}

/// Append one line to `path`, rolling it to `path`.1 once it passes `max_bytes`.
inline void au_resize_trace_append(const std::string& path, const std::string& line,
                                   long max_bytes = kAuResizeTraceMaxBytes) {
    struct stat st{};
    if (::stat(path.c_str(), &st) == 0 && st.st_size + static_cast<long>(line.size()) > max_bytes)
        std::rename(path.c_str(), (path + ".1").c_str());
    if (std::FILE* f = std::fopen(path.c_str(), "a")) {
        std::fwrite(line.data(), 1, line.size(), f);
        std::fclose(f);
    }
}

inline void au_resize_trace_write(const char* format, ...) __attribute__((format(printf, 1, 2)));
inline void au_resize_trace_write(const char* format, ...) {
    char message[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof message, format, args);
    va_end(args);

    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() %
        1000;
    std::tm local{};
    ::localtime_r(&seconds, &local);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &local);
    char line[640];
    std::snprintf(line, sizeof line, "%s.%03d pid=%d [pulp-auv2-resize] %s\n", stamp,
                  static_cast<int>(ms), static_cast<int>(::getpid()), message);

    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    std::fputs(line, stderr);
    const auto& config = au_resize_trace_config();
    if (!config.log_path.empty())
        au_resize_trace_append(config.log_path, line);
}

} // namespace pulp::format::detail

#define PULP_AU_RESIZE_TRACE(...)                                                                  \
    do {                                                                                           \
        if (::pulp::format::detail::au_resize_trace_enabled())                                     \
            ::pulp::format::detail::au_resize_trace_write(__VA_ARGS__);                            \
    } while (0)
