#pragma once

// PulpHostBench — per-instance event logger.
//
// One file per (host, format, pid, timestamp) tuple, written under
//
//   macOS:   $HOME/Library/Logs/PulpHostBench/
//   Linux:   $XDG_STATE_HOME/pulp-host-bench/  (falls back to ~/.local/state/...)
//   Windows: %LOCALAPPDATA%/PulpHostBench/    (falls back to %USERPROFILE%/...)
//
// Each log line is a single tab-separated record:
//
//   <iso8601-ts>\t<event>\t<key=value>\t<key=value>...
//
// Event names are stable and consumed verbatim by
// `tools/scripts/promote_quirk_tiers.py`, so keep new events additive.

#include <pulp/format/host_type.hpp>
#include <pulp/runtime/log.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#if defined(_WIN32)
#  include <process.h>
#  define PULP_BENCH_GETPID _getpid
#else
#  include <unistd.h>
#  define PULP_BENCH_GETPID ::getpid
#endif

namespace pulp::examples::bench {

inline std::string host_name(format::HostType type) {
    return format::host_type_name(type);
}

inline std::filesystem::path bench_log_dir() {
#if defined(__APPLE__)
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return std::filesystem::path(home) / "Library" / "Logs" / "PulpHostBench";
    }
#elif defined(_WIN32)
    const char* local = std::getenv("LOCALAPPDATA");
    if (local && *local) {
        return std::filesystem::path(local) / "PulpHostBench";
    }
    const char* profile = std::getenv("USERPROFILE");
    if (profile && *profile) {
        return std::filesystem::path(profile) / "PulpHostBench";
    }
#else
    const char* xdg = std::getenv("XDG_STATE_HOME");
    if (xdg && *xdg) {
        return std::filesystem::path(xdg) / "pulp-host-bench";
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return std::filesystem::path(home) / ".local" / "state" / "pulp-host-bench";
    }
#endif
    return std::filesystem::temp_directory_path() / "pulp-host-bench";
}

inline std::string iso8601_now() {
    using clock = std::chrono::system_clock;
    auto now = clock::now();
    auto secs = std::chrono::time_point_cast<std::chrono::seconds>(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - secs).count();
    std::time_t t = clock::to_time_t(secs);
    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    std::ostringstream os;
    os << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S") << '.'
       << std::setw(3) << std::setfill('0') << ms << 'Z';
    return os.str();
}

inline std::string compact_now() {
    using clock = std::chrono::system_clock;
    std::time_t t = clock::to_time_t(clock::now());
    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    std::ostringstream os;
    os << std::put_time(&tm_buf, "%Y%m%dT%H%M%SZ");
    return os.str();
}

/// One logger per processor instance — writes to a unique file so
/// parallel hosts (e.g. AU + VST3 in the same Logic session) don't stomp.
class BenchLogger {
public:
    BenchLogger(std::string format_label, format::HostType host)
        : format_(std::move(format_label)),
          host_(host_name(host)),
          host_type_(host) {
        std::error_code ec;
        auto dir = bench_log_dir();
        std::filesystem::create_directories(dir, ec);

        std::ostringstream filename;
        filename << host_ << '-' << format_ << '-' << compact_now()
                 << "-pid" << PULP_BENCH_GETPID() << ".log";
        path_ = (dir / filename.str()).string();

        out_.open(path_, std::ios::out | std::ios::app);
        write_event("session_start",
                    {{"host", host_},
                     {"format", format_},
                     {"pulp_bench_plugin", "1.0.0"}});
    }

    ~BenchLogger() {
        write_event("session_end", {});
    }

    BenchLogger(const BenchLogger&) = delete;
    BenchLogger& operator=(const BenchLogger&) = delete;

    using KV = std::pair<std::string, std::string>;
    using KVList = std::initializer_list<KV>;

    /// Append one event. Thread-safe — called from audio thread (process)
    /// and host thread (prepare/setState/…). The audio-thread path keeps
    /// allocations on the stack-ish std::ostringstream but does take the
    /// mutex; this is acceptable because the harness is a *bench* tool
    /// (not a shipping plugin) and the user runs it deliberately.
    void write_event(std::string_view event, KVList kvs) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!out_.is_open()) return;
        out_ << iso8601_now() << '\t' << event;
        for (auto const& kv : kvs) {
            out_ << '\t' << kv.first << '=' << kv.second;
        }
        out_ << '\t' << "n=" << ++event_count_ << '\n';
        out_.flush();
        last_event_ = std::string(event);
    }

    std::string last_event() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return last_event_;
    }

    std::uint64_t event_count() const {
        return event_count_.load(std::memory_order_relaxed);
    }

    const std::string& path() const { return path_; }
    format::HostType host_type() const { return host_type_; }
    const std::string& format() const { return format_; }
    const std::string& host() const { return host_; }

    // Helpers for common stringifications.
    template <typename T>
    static std::string to_str(T const& v) {
        std::ostringstream os;
        os << v;
        return os.str();
    }
    static std::string bool_str(bool b) { return b ? "true" : "false"; }

private:
    mutable std::mutex mtx_;
    std::ofstream out_;
    std::string path_;
    std::string format_;
    std::string host_;
    format::HostType host_type_;
    std::string last_event_;
    std::atomic<std::uint64_t> event_count_{0};
};

}  // namespace pulp::examples::bench
